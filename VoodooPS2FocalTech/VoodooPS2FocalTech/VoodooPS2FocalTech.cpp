/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "Library/LegacyIOService.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include "VoodooPS2Controller/VoodooPS2Controller.h"
#include "VoodooPS2FocalTech.hpp"
#include "Multitouch Support/VoodooPS2DigitiserTransducer.hpp"

// =============================================================================
// ApplePS2FocalTechTouchPad Class Implementation
//

OSDefineMetaClassAndStructors(ApplePS2FocalTechTouchPad, IOHIPointing);

UInt32 ApplePS2FocalTechTouchPad::deviceType()
{ return NX_EVS_DEVICE_TYPE_MOUSE; };

UInt32 ApplePS2FocalTechTouchPad::interfaceID()
{ return NX_EVS_DEVICE_INTERFACE_BUS_ACE; };

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2FocalTechTouchPad::init(OSDictionary * dict)
{
    //
    // Initialize this object's minimal state. This is invoked right after this
    // object is instantiated.
    //
    
    if (!super::init(dict))
        return false;
    
    transducers = OSArray::withCapacity(FOCALTECH_MAX_FINGERS);
    if (!transducers) {
        return false;
    }
    DigitiserTransducerType type = kDigitiserTransducerFinger;
    for (int i = 0; i < FOCALTECH_MAX_FINGERS; i++) {
        VoodooPS2DigitiserTransducer* transducer = VoodooPS2DigitiserTransducer::transducer(type, NULL);
        transducers->setObject(transducer);
        transducer->release();
    }
    
    // initialize state...
    _device                    = 0;
    _interruptHandlerInstalled = false;
    _packetByteCount           = 0;
    keytime                    = 0;
    maxaftertyping             = 0;
    
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ApplePS2FocalTechTouchPad* ApplePS2FocalTechTouchPad::probe(IOService * provider, SInt32 * score)
{
    
    IOLog("%s :: probe entered...\n", getName());
    
    if (!super::probe(provider, score))
        return 0;
    
    //  Read QuietTimeAfterTyping configuration value
    OSNumber* quiet_time_after_typing = OSDynamicCast(OSNumber, getProperty("QuietTimeAfterTyping"));
    if(quiet_time_after_typing != NULL)
        maxaftertyping = quiet_time_after_typing->unsigned64BitValue() * 1000000;
    
    //
    // The driver has been instructed to verify the presence of the actual
    // hardware we represent. We are guaranteed by the controller that the
    // mouse clock is enabled and the mouse itself is disabled (thus it
    // won't send any asynchronous mouse data that may mess up the
    // responses expected by the commands we send it).
    //
    
    bool success = false;
    
    _device = (ApplePS2MouseDevice *) provider;
    
    
    ApplePS2FocalTechTouchPad::getProductID(&bytes);
    
    if(bytes.byte0 == 0x6b && bytes.byte1 == 0x80 && bytes.byte2 == 0x80)
        success = true;
    
    IOLog("%s :: FLT0102 ? %s\n", getName(), (success ? "yes" : "no"));
    
    _device = 0;
    
    IOLog("%s :: probe leaving.\n", getName());
    
    return (success) ? this : 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2FocalTechTouchPad::start(IOService * provider)
{
    //
    // The driver has been instructed to start. This is called after a
    // successful probe and match.
    //
    
    if (!super::start(provider))
        return false;
    
    //
    // Maintain a pointer to and retain the provider object.
    //
    
    _device = (ApplePS2MouseDevice *) provider;
    _device->retain();
    
    //
    // Lock the controller during initialization
    //
    
    _device->lock();
    
    // reset device
    
    doHardwareReset();
    
    // Switch TouchPad Protocol
    
    switchProtocol();
    
    // MultiTouch Interface
    
    publish_multitouch_interface();
    init_multitouch_interface();
    
    //
    // Finally, we enable the trackpad itself, so that it may start reporting
    // asynchronous events.
    //
    
    setTouchPadEnable(true);
    
    //
    // Enable the mouse clock (should already be so) and the mouse IRQ line.
    //
    
    //
    // Install our driver's interrupt handler, for asynchronous data delivery.
    //
    
    _device->installInterruptAction(this, OSMemberFunctionCast(PS2InterruptAction, this, &ApplePS2FocalTechTouchPad::interruptOccurred), OSMemberFunctionCast(PS2PacketAction, this, &ApplePS2FocalTechTouchPad::packetReady));
    
    _interruptHandlerInstalled = true;
    
    
    
    // now safe to allow other threads
    _device->unlock();
    
    //
    // Install our power control handler.
    //
    
    _device->installPowerControlAction( this, OSMemberFunctionCast(PS2PowerControlAction,this, &ApplePS2FocalTechTouchPad::setDevicePowerState));
    
    _powerControlHandlerInstalled = true;
    
    if(mt_interface) {
        mt_interface->registerService();
    }
    
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::stop( IOService * provider )
{
    //
    // The driver has been instructed to stop.  Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.
    //
    
    assert(_device == provider);
    
    //
    // Disable the mouse itself, so that it may stop reporting mouse events.
    //
    
    setTouchPadEnable(false);
    
    //
    // Uninstall the interrupt handler.
    //
    
    if ( _interruptHandlerInstalled )  _device->uninstallInterruptAction();
    _interruptHandlerInstalled = false;
    
    //
    // Uninstall the power control handler.
    //
    
    if ( _powerControlHandlerInstalled ) _device->uninstallPowerControlAction();
    _powerControlHandlerInstalled = false;
    
    //
    // Release the pointer to the provider object.
    //
    
    OSSafeReleaseNULL(_device);
    
    unpublish_multitouch_interface();
    
    if (transducers){
        OSSafeReleaseNULL(transducers);
    }
    
    super::stop(provider);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2FocalTechTouchPad::publish_multitouch_interface() {
    mt_interface = new VoodooPS2MultitouchInterface();
    if (!mt_interface) {
        IOLog("%s :: No memory to allocate VoodooPS2MultitouchInterface instance\n", getName());
        goto multitouch_exit;
    }
    if (!mt_interface->init(NULL)) {
        IOLog("%s :: Failed to init multitouch interface\n", getName());
        goto multitouch_exit;
    }
    if (!mt_interface->attach(this)) {
        IOLog("%s :: Failed to attach multitouch interface\n", getName());
        goto multitouch_exit;
    }
    if (!mt_interface->start(this)) {
        IOLog("%s :: Failed to start multitouch interface\n", getName());
        goto multitouch_exit;
    }
    // Assume we are a touchpad
    mt_interface->setProperty(kIOHIDDisplayIntegratedKey, false);
    // 0x04f3 is Elan's Vendor Id
    mt_interface->setProperty(kIOHIDVendorIDKey, 0x04f3, 32);
    mt_interface->setProperty(kIOHIDProductIDKey, 0x01, 32);
    
    
    return true;
multitouch_exit:
    unpublish_multitouch_interface();
    return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::unpublish_multitouch_interface() {
    if (mt_interface) {
        mt_interface->stop(this);
        mt_interface->detach(this);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool ApplePS2FocalTechTouchPad::init_multitouch_interface() {
    if(mt_interface){
        mt_interface->physical_max_x = PHYSCICAL_MAX_X;
        mt_interface->physical_max_y = PHYSCICAL_MAX_Y;
        mt_interface->logical_max_x  = LOGICAL_MAX_X;
        mt_interface->logical_max_y  = LOGICAL_MAX_Y;
    }
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PS2InterruptResult ApplePS2FocalTechTouchPad::interruptOccurred(UInt8 data)
{
    //
    // This will be invoked automatically from our device when asynchronous
    // events need to be delivered. Process the trackpad data. Do NOT issue
    // any BLOCKING commands to our device in this context.
    //
    // Ignore all bytes until we see the start of a packet, otherwise the
    // packets may get out of sequence and things will get very confusing.
    //
    
    if (0 == _packetByteCount && (data & 0xf) != 0x3 && (data & 0xf) != 0x6 && (data & 0xf) != 0x9)
    {
        IOLog("%s :: Unexpected byte0 data (%02x) from PS/2 controller\n", getName(), data);
        return kPS2IR_packetBuffering;
    }
    
    UInt8* packet = _ringBuffer.head();
    packet[_packetByteCount++] = data;
    
    if (kPacketLengthMax == _packetByteCount)
    {
        _ringBuffer.advanceHead(kPacketLengthMax);
        _packetByteCount = 0;
        return kPS2IR_packetReady;
    }
    return kPS2IR_packetBuffering;
}

void ApplePS2FocalTechTouchPad::packetReady()
{
    // empty the ring buffer, dispatching each packet...
    while (_ringBuffer.count() >= kPacketLengthMax)
    {
        UInt8* packet = _ringBuffer.tail();
        // now we have complete packet
        parsePacket(packet);
        _ringBuffer.advanceTail(kPacketLengthMax);
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::parsePacket(UInt8* packet)
{
    switch (packet[0] & 0xf) {
        case 0x3:
            process_touch_packet(packet);
            break;
                
        case 0x6:
            process_abs_packet(packet);
            break;
                
        case 0x9:
            process_rel_packet(packet);
            break;
    }
        
    sendTouchDataToMultiTouchInterface();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::process_touch_packet(UInt8 *packet)
{
    unsigned char fingers = packet[1];
    int i;
    
    State.pressed = (packet[0] >> 4) & 1;
    
    /* the second byte contains a bitmap of all fingers touching the pad */
    for (i = 0; i < FOCALTECH_MAX_FINGERS; i++) {
        State.fingers[i].active = fingers & 0x1;
        if (!State.fingers[i].active) {
            /*
             * Even when the finger becomes active again, we still
             * will have to wait for the first valid position.
             */
            State.fingers[i].valid = false;
        }
        fingers >>= 1;
    }
}

void ApplePS2FocalTechTouchPad::process_abs_packet(UInt8 *packet)
{
    focaltech_hw_state *state = &State;
    unsigned int finger;
    
    finger = (packet[1] >> 4) - 1;
    if (finger >= FOCALTECH_MAX_FINGERS) {
        IOLog("Invalid finger in abs packet: %d\n",finger);
        return;
    }
    
    state->pressed = (packet[0] >> 4) & 1;
    
    state->fingers[finger].x = ((packet[1] & 0xf) << 8) | packet[2];
    state->fingers[finger].y = (packet[3] << 8) | packet[4];
    state->width = packet[5] >> 4;
    state->fingers[finger].valid = true;
}

void ApplePS2FocalTechTouchPad::process_rel_packet(UInt8 *packet)
{
    focaltech_hw_state *state = &State;
    int finger1, finger2;
    
    state->pressed = packet[0] >> 7;
    finger1 = ((packet[0] >> 4) & 0x7) - 1;
    if (finger1 < FOCALTECH_MAX_FINGERS) {
        state->fingers[finger1].x += (char)packet[1];
        state->fingers[finger1].y += (char)packet[2];
    } else {
        IOLog("First finger in rel packet invalid: %d\n",
                    finger1);
    }
    
    /*
     * If there is an odd number of fingers, the last relative
     * packet only contains one finger. In this case, the second
     * finger index in the packet is 0 (we subtract 1 in the lines
     * above to create array indices, so the finger will overflow
     * and be above FOC_MAX_FINGERS).
     */
    finger2 = ((packet[3] >> 4) & 0x7) - 1;
    if (finger2 < FOCALTECH_MAX_FINGERS) {
        state->fingers[finger2].x += (char)packet[4];
        state->fingers[finger2].y += (char)packet[5];
    }
}
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::sendTouchDataToMultiTouchInterface() {
    if(!mt_interface)
        return;
    
    UInt32 buttons = State.pressed ? 1:0;
    
    AbsoluteTime timestamp;
    clock_get_uptime(&timestamp);
    uint64_t timestamp_ns;
    absolutetime_to_nanoseconds(timestamp, &timestamp_ns);
    
    if ((maxaftertyping > 0) && (timestamp_ns - keytime < maxaftertyping))
        return;
    
    int count = 0;
    for (int i = 0; i < FOCALTECH_MAX_FINGERS; i++) {
        focaltech_finger_state *state = State.fingers+i;
        VoodooPS2DigitiserTransducer* transducer = OSDynamicCast(VoodooPS2DigitiserTransducer, transducers->getObject(i));
        transducer->type = kDigitiserTransducerFinger;
        if(!transducer)
            continue;
        bool valid = state->active && state->valid;
        transducer->is_valid = valid;
        
        if(valid){
            if(mt_interface){
                transducer->logical_max_x = mt_interface->logical_max_x;
                transducer->logical_max_y = mt_interface->logical_max_y;
            }
            unsigned int clamped_x, clamped_y;
            clamped_x = clamp(state->x, 0U, LOGICAL_MAX_X);
            clamped_y = clamp(state->y, 0U, LOGICAL_MAX_Y);
            transducer->coordinates.x.update(clamped_x, timestamp);
            transducer->coordinates.y.update(LOGICAL_MAX_Y-clamped_y, timestamp);
            transducer->tip_switch.update(1, timestamp);
            transducer->id = i;
            transducer->secondary_id = i;
            count++;
        } else{
            transducer->id = i;
            transducer->secondary_id = i;
            transducer->coordinates.x.update(transducer->coordinates.x.last.value, timestamp);
            transducer->coordinates.y.update(transducer->coordinates.y.last.value, timestamp);
            transducer->tip_switch.update(0, timestamp);
        }
    }
    VoodooI2CMultitouchEvent event;
    event.contact_count = count;
    event.transducers = transducers;
    if (mt_interface){
        mt_interface->handleInterruptReport(event, timestamp);
        dispatchRelativePointerEvent(0, 0, buttons, timestamp);
        
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::setTouchPadEnable( bool enable )
{
    //
    // Instructs the trackpad to start or stop the reporting of data packets.
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    TPS2Request<1> request;
    
    // (mouse or pad enable/disable command)
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = enable ? kDP_Enable : kDP_SetDefaultsAndDisable;
    request.commandsCount = 1;
    assert(request.commandsCount <= countof(request.commands));
    _device->submitRequestAndBlock(&request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::setDevicePowerState( UInt32 whatToDo )
{
    switch ( whatToDo )
    {
        case kPS2C_DisableDevice:
            
            //
            // Disable touchpad.
            //
            setTouchPadEnable( false );
            break;
            
        case kPS2C_EnableDevice:
            
            //
            // Finally, we enable the trackpad itself, so that it may
            // start reporting asynchronous events.
            //
            
            _ringBuffer.reset();
            _packetByteCount = 0;
            
            setTouchPadEnable(true);
            break;
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::doHardwareReset()
{
    TPS2Request<4> request;
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = kDP_SetDefaults;
    request.commands[1].command = kPS2C_SendCommandAndCompareAck;
    request.commands[1].inOrOut = kDP_Reset;
    request.commands[2].command = kPS2C_ReadDataPortAndCompare;
    request.commands[2].inOrOut = 0xAA;
    request.commands[3].command = kPS2C_ReadDataPortAndCompare;
    request.commands[3].inOrOut = 0x00;
    
    request.commandsCount = 4;
    IOLog("%s :: sending kDP_Reset $FF\n", getName());
    assert(request.commandsCount <= countof(request.commands));
    _device->submitRequestAndBlock(&request);
   
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void ApplePS2FocalTechTouchPad::switchProtocol()
{
    TPS2Request<10> request;
    
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = 0xF8;
    request.commands[1].command = kPS2C_SendCommandAndCompareAck;
    request.commands[1].inOrOut = 0;
    
    request.commands[2].command = kPS2C_SendCommandAndCompareAck;
    request.commands[2].inOrOut = 0xF8;
    request.commands[3].command = kPS2C_SendCommandAndCompareAck;
    request.commands[3].inOrOut = 0;
    
    request.commands[4].command = kPS2C_SendCommandAndCompareAck;
    request.commands[4].inOrOut = 0xF8;
    request.commands[5].command = kPS2C_SendCommandAndCompareAck;
    request.commands[5].inOrOut = 0;
    
    request.commands[6].command = kPS2C_SendCommandAndCompareAck;
    request.commands[6].inOrOut = 0xF8;
    request.commands[7].command = kPS2C_SendCommandAndCompareAck;
    request.commands[7].inOrOut = 1;
    
    request.commands[8].command = kPS2C_SendCommandAndCompareAck;
    request.commands[8].inOrOut = kDP_SetMouseScaling1To1;
    
    request.commands[9].command = kPS2C_SendCommandAndCompareAck;
    request.commands[9].inOrOut = kDP_Enable;
    
    request.commandsCount = 10;
    assert(request.commandsCount <= countof(request.commands));
    _device->submitRequestAndBlock(&request);
}

void ApplePS2FocalTechTouchPad::getProductID(FTE_BYTES_t *bytes){
    
    TPS2Request<13> request;
    request.commands[0].command = kPS2C_SendCommandAndCompareAck;
    request.commands[0].inOrOut = kDP_SetMouseScaling1To1;
    
    request.commands[1].command = kPS2C_SendCommandAndCompareAck;
    request.commands[1].inOrOut = kDP_SetMouseResolution;
    request.commands[2].command = kPS2C_SendCommandAndCompareAck;
    request.commands[2].inOrOut = 0;
    
    request.commands[3].command = kPS2C_SendCommandAndCompareAck;
    request.commands[3].inOrOut = kDP_SetMouseResolution;
    request.commands[4].command = kPS2C_SendCommandAndCompareAck;
    request.commands[4].inOrOut = 0;
    
    request.commands[5].command = kPS2C_SendCommandAndCompareAck;
    request.commands[5].inOrOut = kDP_SetMouseResolution;
    request.commands[6].command = kPS2C_SendCommandAndCompareAck;
    request.commands[6].inOrOut = 0;
    
    request.commands[7].command = kPS2C_SendCommandAndCompareAck;
    request.commands[7].inOrOut = kDP_SetMouseResolution;
    request.commands[8].command = kPS2C_SendCommandAndCompareAck;
    request.commands[8].inOrOut = 0;
    
    request.commands[9].command  = kPS2C_SendCommandAndCompareAck;
    request.commands[9].inOrOut  = kDP_GetMouseInformation;
    request.commands[10].command  = kPS2C_ReadDataPort;
    request.commands[10].inOrOut  = 0;
    request.commands[11].command = kPS2C_ReadDataPort;
    request.commands[11].inOrOut = 0;
    request.commands[12].command = kPS2C_ReadDataPort;
    request.commands[12].inOrOut = 0;
    request.commandsCount = 13;
    assert(request.commandsCount <= countof(request.commands));
    
    _device->submitRequestAndBlock(&request);
    
    bytes->byte0 = request.commands[10].inOrOut; // 0x6b
    bytes->byte1 = request.commands[11].inOrOut; // 0x80
    bytes->byte2 = request.commands[12].inOrOut; // 0x80
   
    
    
    
    IOLog("%s :: Product ID: [%02x %02x %02x]\n", getName(), bytes->byte0, bytes->byte1, bytes->byte2);
   
}

IOReturn ApplePS2FocalTechTouchPad::message(UInt32 type, IOService* provider, void* argument) {
    // Here is where we receive messages from the keyboard driver
    //
    //  It allows the trackpad driver to learn the last time a key
    //  has been pressed, so it can implement various "ignore trackpad
    //  input while typing" options.
    //
    if(type == kPS2M_notifyKeyPressed){
        // remember last time key pressed... this can be used in
        // interrupt handler to detect unintended input while typing
        
        PS2KeyInfo* pInfo = (PS2KeyInfo*)argument;
        keytime = pInfo->time;
        
        // Perform a manual Reset Touchpad when F7 key is pressed this help
        // when Touchpad device is disabled accidently, Temprary solution until
        // i understand the functionality of OEM build-in Touchpad Disable Key
        
        if(pInfo->goingDown && pInfo->adbKeyCode == 0x62){
            _ringBuffer.reset();
            _packetByteCount = 0;
            _device->lock();
            doHardwareReset();
            switchProtocol();
            setTouchPadEnable(true);
            _device->unlock();
        }
    }
    return kIOReturnSuccess;
}


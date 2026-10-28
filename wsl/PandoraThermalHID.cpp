/*
 * PANDORA: the phone's temperatures for iOS's thermal monitor, thermalmonitord, the way the iPhone X
 * (D22) gets them; which value each sensor carries is bsd/dev/pandora_therm.c's business (pt_hid).
 *
 * thermalmonitord (HidSensors) takes every HID service with PrimaryUsagePage 0xff00 and PrimaryUsage 5
 * (copySensorArray), pairs its "Product" with the matchName of a sensor in the product's table
 * (/System/Library/ThermalMonitor/D22AP-Info.plist, Sensors) and reads it with
 * IOHIDServiceClientCopyEvent(kIOHIDEventTypeTemperature), the level in degrees C (getHIDSensorDict).
 * Sensors with a callbackInterval in the table (mic1, mic2) it takes from the events the service
 * dispatches, after setting the service's ReportInterval. Missing sensors are logged and left out.
 *
 *   PandoraThermalHID      matched on IOResources; publishes one PandoraHIDTemperature per sensor of
 *                          pandora_therm.c (none unless pandora_therm=2 or more)
 *   PandoraHIDTemperature  one sensor: copyEvent answers with pandora_therm_hid_mdeg; callback sensors
 *                          also dispatch an event every ReportInterval
 *   AppleVTempDispatcher   where thermalmonitord sends the temperatures it computes (generalProductConfig
 *                          shouldDispatchVirtualTemp; HidSensors sendVirtualTemp:temperature: sets one
 *                          property, the sensor's 4CC = an int, degrees in 16.16 fixed point rounded from
 *                          its hundredths, with IORegistryEntrySetCFProperty); they go to
 *                          pandora_therm_ios_temp in hundredths. Apple's class of that name
 *                          (AppleEmbeddedTempSensor.kext) is not in this kernel.
 */
#if defined(PANDORA)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventService.h"
#include "IOHIDEvent.h"
#include "IOHIDKeys.h"
#include "IOHIDDeviceKeys.h"

extern "C" {
uint64_t mach_absolute_time(void);
unsigned pandora_therm_hid_count(void);
const char *pandora_therm_hid_name(unsigned i);
int pandora_therm_hid_callback(unsigned i);
int pandora_therm_hid_mdeg(unsigned i, int32_t *mdeg);
void pandora_therm_ios_temp(const char *key, int32_t centi);
}

#define PH_USAGE_PAGE 0xff00u    /* Apple vendor page */
#define PH_USAGE      5u         /* temperature sensor */

class PandoraHIDTemperature : public IOHIDEventService {
	OSDeclareDefaultStructors(PandoraHIDTemperature);
public:
	bool initSensor(unsigned index);
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *matching, IOOptionBits options) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return PH_USAGE_PAGE; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return PH_USAGE; }
	virtual OSString *getProduct() APPLE_KEXT_OVERRIDE { return OSString::withCString(pandora_therm_hid_name(fIndex)); }
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 0x100 + fIndex; }
	virtual UInt32 getReportInterval() APPLE_KEXT_OVERRIDE { return 1000000; }   /* us */
private:
	IOHIDEvent *temperature();
	static void tick(OSObject *owner, IOTimerEventSource *sender);
	unsigned fIndex;
	IOTimerEventSource *fTimer;
	uint32_t fIntervalMS;
};
OSDefineMetaClassAndStructors(PandoraHIDTemperature, IOHIDEventService);

bool
PandoraHIDTemperature::initSensor(unsigned index)
{
	fIndex = index;
	fIntervalMS = 1000;
	return init();
}

bool
PandoraHIDTemperature::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	if (pandora_therm_hid_callback(fIndex)) {
		fTimer = IOTimerEventSource::timerEventSource(this, &PandoraHIDTemperature::tick);
		if (!fTimer || _hidWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
			OSSafeReleaseNULL(fTimer);
			return false;
		}
		fTimer->setTimeoutMS(fIntervalMS);
	}
	registerService();
	return true;
}

void
PandoraHIDTemperature::free(void)
{
	if (fTimer) {
		fTimer->cancelTimeout();
		if (_hidWorkLoop) {
			_hidWorkLoop->removeEventSource(fTimer);
		}
		OSSafeReleaseNULL(fTimer);
	}
	IOHIDEventService::free();
}

IOHIDEvent *
PandoraHIDTemperature::temperature()
{
	int32_t mdeg;
	if (!pandora_therm_hid_mdeg(fIndex, &mdeg)) {
		return NULL;
	}
	return IOHIDEvent::temperatureEvent(mach_absolute_time(), (IOFixed)(((int64_t)mdeg << 16) / 1000));
}

IOHIDEvent *
PandoraHIDTemperature::copyEvent(IOHIDEventType type, IOHIDEvent *matching __unused, IOOptionBits options __unused)
{
	return type == kIOHIDEventTypeTemperature ? temperature() : NULL;
}

/* thermalmonitord sets ReportInterval (us) on its callback sensors; everything else goes to the base. */
IOReturn
PandoraHIDTemperature::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	OSNumber *interval = dict ? OSDynamicCast(OSNumber, dict->getObject(kIOHIDReportIntervalKey)) : NULL;
	if (interval) {
		uint64_t us = interval->unsigned64BitValue();
		uint32_t ms = (uint32_t)(us / 1000);
		ms = ms < 100 ? 100 : (ms > 10000 ? 10000 : ms);
		IOLog("PANDORA THERM HID: %s ReportInterval %llu us -> every %u ms\n", pandora_therm_hid_name(fIndex), us, ms);
		fIntervalMS = ms;
		setProperty(kIOHIDReportIntervalKey, us, 32);
	}
	return IOHIDEventService::setProperties(properties);
}

void
PandoraHIDTemperature::tick(OSObject *owner, IOTimerEventSource *sender)
{
	PandoraHIDTemperature *self = (PandoraHIDTemperature *)owner;
	IOHIDEvent *event = self->temperature();
	if (event) {
		self->dispatchEvent(event);
		event->release();
	}
	sender->setTimeoutMS(self->fIntervalMS);
}

class PandoraThermalHID : public IOService {
	OSDeclareDefaultStructors(PandoraThermalHID);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PandoraThermalHID, IOService);

bool
PandoraThermalHID::start(IOService *provider)
{
	const unsigned n = pandora_therm_hid_count();
	if (!n || !IOService::start(provider)) {
		return false;
	}
	unsigned up = 0;
	for (unsigned i = 0; i < n; i++) {
		PandoraHIDTemperature *sensor = OSTypeAlloc(PandoraHIDTemperature);
		if (sensor && sensor->initSensor(i) && sensor->attach(this)) {
			if (sensor->start(this)) {
				up++;
			} else {
				sensor->detach(this);
			}
		}
		OSSafeReleaseNULL(sensor);
	}
	registerService();
	IOLog("PANDORA THERM HID: %u of %u temperature sensors published\n", up, n);
	return true;
}

class AppleVTempDispatcher : public IOService {
	OSDeclareDefaultStructors(AppleVTempDispatcher);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleVTempDispatcher, IOService);

bool
AppleVTempDispatcher::start(IOService *provider)
{
	if (!pandora_therm_hid_count() || !IOService::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

IOReturn
AppleVTempDispatcher::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	if (!dict) {
		return kIOReturnBadArgument;
	}
	OSCollectionIterator *keys = OSCollectionIterator::withCollection(dict);
	if (!keys) {
		return kIOReturnNoMemory;
	}
	while (OSSymbol *key = OSDynamicCast(OSSymbol, keys->getNextObject())) {
		OSNumber *value = OSDynamicCast(OSNumber, dict->getObject(key));
		if (value) {
			const int64_t fixed = (int32_t)value->unsigned32BitValue();    /* 16.16 */
			pandora_therm_ios_temp(key->getCStringNoCopy(), (int32_t)(fixed * 100 / 65536));
		}
	}
	keys->release();
	return kIOReturnSuccess;
}
#endif /* PANDORA */

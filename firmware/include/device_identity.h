// device_identity.h — stable device identity (Phase 7 topics, HA ids).
//
// SPEC §4.10: device_id is stable across reboots and firmware updates so
// MQTT topics and Home Assistant unique_ids never change. Generated once
// from the chip MAC + a monotonic counter, stored in NVS, and readable by
// any module (web /device, MQTT, network storage).

#pragma once

namespace visioncam {

// Returns the stable device id (never null; "vc0000000000" on NVS fault).
const char* deviceIdentityId();

}  // namespace visioncam
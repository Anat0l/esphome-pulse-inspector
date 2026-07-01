#pragma once

#include "pulse_inspector_exe.h"

#ifdef USE_ESP_IDF

#include <vector>

#include "esphome/core/automation.h"

namespace esphome {
namespace pulse_inspector_exe {

// Triggers fired on every fully decoded master/slave frame. The frame
// body is exposed as `data` (std::vector<uint8_t>), and length /
// suppression flag as additional parameters so YAML can write
//
//   on_master_frame:
//     then:
//       - lambda: |-
//           if (len >= 2 && data[0] == 0x00 && data[1] == 0xFE) return;
//           ESP_LOGI("exe", "master frame: %u bytes", (unsigned) len);
//
// Mode-bits per byte are exposed via `mode` (same length as `data`).
// `suppressed=true` means the frame matched an idle pattern and was
// hidden from the regular log; the trigger still fires so user code
// can decide independently.
class MasterFrameTrigger
    : public Trigger<std::vector<uint8_t>, std::vector<uint8_t>, size_t, bool> {
 public:
  explicit MasterFrameTrigger(PulseInspectorExe *parent) {
    parent->add_on_master_frame_callback([this](const ExeFrameEvent &e) {
      std::vector<uint8_t> data(e.data, e.data + e.len);
      std::vector<uint8_t> mode(e.mode, e.mode + e.len);
      this->trigger(std::move(data), std::move(mode), (size_t) e.len, e.suppressed);
    });
  }
};

class SlaveFrameTrigger
    : public Trigger<std::vector<uint8_t>, std::vector<uint8_t>, size_t, bool> {
 public:
  explicit SlaveFrameTrigger(PulseInspectorExe *parent) {
    parent->add_on_slave_frame_callback([this](const ExeFrameEvent &e) {
      std::vector<uint8_t> data(e.data, e.data + e.len);
      std::vector<uint8_t> mode(e.mode, e.mode + e.len);
      this->trigger(std::move(data), std::move(mode), (size_t) e.len, e.suppressed);
    });
  }
};

// ============================================================================
// Semantic-level triggers
//
// Fire on decoded application-layer events instead of raw byte frames:
//
//   - on_vend_complete     : VMC replied to 0x33 VEND. Args: ok (bool),
//                            value_real_money (uint32_t), selection_price
//                            (uint8_t), audit_pairs_pending (uint8_t).
//
//   - on_credit_change     : VMC ACCEPT DATA block decoded. Args:
//                            credit (uint32_t), delta (int32_t),
//                            base_units (uint16_t), exact_change_only
//                            (bool).
//
//   - on_vmc_status        : VMC STATUS reply changed. Args: status_raw
//                            (uint8_t), vending_inhibited (bool),
//                            free_vend_request (bool),
//                            audit_pairs_pending (uint8_t).
// ============================================================================

class VendCompleteTrigger
    : public Trigger<bool, uint32_t, uint8_t, uint8_t> {
 public:
  explicit VendCompleteTrigger(PulseInspectorExe *parent) {
    parent->add_on_vend_complete_callback([this](const ExeVendEvent &e) {
      this->trigger(e.ok, e.value_real_money, e.selection_price,
                    e.audit_pairs_pending);
    });
  }
};

class CreditChangeTrigger
    : public Trigger<uint32_t, int32_t, uint16_t, bool> {
 public:
  explicit CreditChangeTrigger(PulseInspectorExe *parent) {
    parent->add_on_credit_change_callback([this](const ExeCreditEvent &e) {
      this->trigger(e.credit_real_money, e.delta_real_money, e.base_units,
                    e.exact_change_only);
    });
  }
};

class VmcStatusTrigger
    : public Trigger<uint8_t, bool, bool, uint8_t> {
 public:
  explicit VmcStatusTrigger(PulseInspectorExe *parent) {
    parent->add_on_vmc_status_callback([this](const ExeStatusEvent &e) {
      this->trigger(e.status_raw, e.vending_inhibited, e.free_vend_request,
                    e.audit_pairs_pending);
    });
  }
};

}  // namespace pulse_inspector_exe
}  // namespace esphome

#endif  // USE_ESP_IDF

#pragma once

#include "pulse_inspector_mdb.h"

#ifdef USE_ESP_IDF

#include "esphome/core/automation.h"

#include <string>

namespace esphome {
namespace pulse_inspector_mdb {

// -----------------------------------------------------------------------------
// Triggers for MDB semantic events.
//
// Each trigger subscribes to the parent PulseInspectorMdb via add_on_event
// and fires when the relevant MdbEvent::Kind is emitted. Trigger argument
// lists are tuned so YAML lambdas can pick out the interesting fields with
// minimal boilerplate.
// -----------------------------------------------------------------------------

// Generic "fires for every decoded event" trigger. Lambda gets the
// human-readable description string.
class EventTrigger : public Trigger<std::string> {
 public:
  explicit EventTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      this->trigger(std::string(e.description));
    });
  }
};

// User pressed a drink button; VMC is asking cashless to pay.
// Args: price_cents, item_number.
class VendRequestTrigger : public Trigger<uint32_t, uint32_t> {
 public:
  explicit VendRequestTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::VEND_REQUEST) {
        this->trigger(e.price_cents, e.item_number);
      }
    });
  }
};

// Cashless approved the VEND. Args: price_cents.
class VendApprovedTrigger : public Trigger<uint32_t> {
 public:
  explicit VendApprovedTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::VEND_APPROVED) {
        this->trigger(e.price_cents);
      }
    });
  }
};

// Cashless refused. No args.
class VendDeniedTrigger : public Trigger<> {
 public:
  explicit VendDeniedTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::VEND_DENIED) {
        this->trigger();
      }
    });
  }
};

// Drink was dispensed OK. Args: item_number.
class VendSuccessTrigger : public Trigger<uint32_t> {
 public:
  explicit VendSuccessTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::VEND_SUCCESS) {
        this->trigger(e.item_number);
      }
    });
  }
};

// Drink was NOT dispensed (no water / no beans / jam). Args: item_number.
class VendFailureTrigger : public Trigger<uint32_t> {
 public:
  explicit VendFailureTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::VEND_FAILURE) {
        this->trigger(e.item_number);
      }
    });
  }
};

// User paid with coins/bills (VMC informs cashless for statistics).
// Args: price_cents, item_number.
class CashSaleTrigger : public Trigger<uint32_t, uint32_t> {
 public:
  explicit CashSaleTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::CASH_SALE) {
        this->trigger(e.price_cents, e.item_number);
      }
    });
  }
};

// Cashless session opened (card presented / credit loaded).
// Args: funds_cents.
class SessionBeginTrigger : public Trigger<uint32_t> {
 public:
  explicit SessionBeginTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::BEGIN_SESSION) {
        this->trigger(e.funds_cents);
      }
    });
  }
};

// Cashless session ended.
class SessionEndTrigger : public Trigger<> {
 public:
  explicit SessionEndTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::END_SESSION ||
          e.kind == MdbEvent::SESSION_CANCEL_REQUEST) {
        this->trigger();
      }
    });
  }
};

// Bill validator fault or status change. Args: byte_code, description.
class BvErrorTrigger : public Trigger<uint8_t, std::string> {
 public:
  explicit BvErrorTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::BV_ERROR || e.kind == MdbEvent::BV_STATUS ||
          e.kind == MdbEvent::BV_RESET) {
        this->trigger(e.byte_code, std::string(e.description));
      }
    });
  }
};

// BV accepted a bill. Args: bill_type, to_stacker (bool).
class BillAcceptedTrigger : public Trigger<uint8_t, bool> {
 public:
  explicit BillAcceptedTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::BV_BILL_ACCEPTED) {
        this->trigger(e.bill_type, e.routed_to_stacker);
      }
    });
  }
};

// Coin changer fault or status. Args: byte_code, description.
class ChangerErrorTrigger : public Trigger<uint8_t, std::string> {
 public:
  explicit ChangerErrorTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::CHG_ERROR || e.kind == MdbEvent::CHG_RESET) {
        this->trigger(e.byte_code, std::string(e.description));
      }
    });
  }
};

// Coin inserted. Args: coin_type, count_in_tube.
class CoinDepositedTrigger : public Trigger<uint8_t, uint8_t> {
 public:
  explicit CoinDepositedTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::CHG_COIN_DEPOSITED) {
        this->trigger(e.coin_type, e.coin_count);
      }
    });
  }
};

// Sale cycle started: the MDB host (coin mech) just disabled the bill
// validator, which on Necta machines is the host's way of locking cash
// input for the duration of a drink being prepared. No args.
class SaleCycleBeginTrigger : public Trigger<> {
 public:
  explicit SaleCycleBeginTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::SALE_CYCLE_BEGIN) {
        this->trigger();
      }
    });
  }
};

// Sale cycle finished: bill validator re-enabled. Arg: duration in ms
// between SALE_CYCLE_BEGIN and SALE_CYCLE_END.
class SaleCycleEndTrigger : public Trigger<uint32_t> {
 public:
  explicit SaleCycleEndTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::SALE_CYCLE_END) {
        this->trigger(e.data32);
      }
    });
  }
};

// Cashless reader malfunction. Args: byte_code, description.
class CashlessErrorTrigger : public Trigger<uint8_t, std::string> {
 public:
  explicit CashlessErrorTrigger(PulseInspectorMdb *parent) {
    parent->add_on_event_callback([this](const MdbEvent &e) {
      if (e.kind == MdbEvent::CL_MALFUNCTION || e.kind == MdbEvent::CL_RESET) {
        this->trigger(e.byte_code, std::string(e.description));
      }
    });
  }
};

}  // namespace pulse_inspector_mdb
}  // namespace esphome

#endif  // USE_ESP_IDF

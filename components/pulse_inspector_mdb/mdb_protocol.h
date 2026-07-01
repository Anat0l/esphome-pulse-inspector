#pragma once

#ifdef USE_ESP_IDF

#include <cstdint>
#include <cstddef>

namespace esphome {
namespace pulse_inspector_mdb {

// ---------------------------------------------------------------------------
// MDB peripheral address groups (top 5 bits of the first command byte)
// ---------------------------------------------------------------------------
enum : uint8_t {
  MDB_ADDR_VMC = 0x00,
  MDB_ADDR_CHANGER = 0x08,
  MDB_ADDR_CASHLESS1 = 0x10,
  MDB_ADDR_COMMS = 0x18,
  MDB_ADDR_DISPLAY = 0x20,
  MDB_ADDR_ENERGY = 0x28,
  MDB_ADDR_BV = 0x30,
  MDB_ADDR_USD1 = 0x40,
  MDB_ADDR_AGE = 0x48,
  MDB_ADDR_USD2 = 0x58,
  MDB_ADDR_CASHLESS2 = 0x60,
  MDB_ADDR_EXPERIMENTAL = 0x70,
};

// Peripheral sub-commands (low 3 bits).
enum : uint8_t {
  // Changer
  MDB_CHG_RESET = 0x00,
  MDB_CHG_SETUP = 0x01,
  MDB_CHG_TUBE_STATUS = 0x02,
  MDB_CHG_POLL = 0x03,
  MDB_CHG_COIN_TYPE = 0x04,
  MDB_CHG_DISPENSE = 0x05,
  MDB_CHG_EXPANSION = 0x07,

  // Bill Validator
  MDB_BV_RESET = 0x00,
  MDB_BV_SETUP = 0x01,
  MDB_BV_SECURITY = 0x02,
  MDB_BV_POLL = 0x03,
  MDB_BV_BILL_TYPE = 0x04,
  MDB_BV_ESCROW = 0x05,
  MDB_BV_STACKER = 0x06,
  MDB_BV_EXPANSION = 0x07,

  // Cashless (1 and 2 share the same sub-command scheme)
  MDB_CL_RESET = 0x00,
  MDB_CL_SETUP = 0x01,
  MDB_CL_POLL = 0x02,
  MDB_CL_VEND = 0x03,
  MDB_CL_READER = 0x04,
  MDB_CL_REVALUE = 0x05,
  MDB_CL_EXPANSION = 0x07,
};

// Cashless VEND sub-subcommands (second byte of a VEND command/reply).
enum : uint8_t {
  MDB_CL_VEND_REQUEST = 0x00,
  MDB_CL_VEND_CANCEL = 0x01,
  MDB_CL_VEND_SUCCESS = 0x02,
  MDB_CL_VEND_FAILURE = 0x03,
  MDB_CL_VEND_SESSION_COMPLETE = 0x04,
  MDB_CL_VEND_CASH_SALE = 0x05,
  MDB_CL_VEND_NEGATIVE = 0x06,
};

// Cashless POLL reply codes (first byte of a POLL reply).
enum : uint8_t {
  MDB_CL_REPLY_JUST_RESET = 0x00,
  MDB_CL_REPLY_CONFIG = 0x01,
  MDB_CL_REPLY_DISPLAY = 0x02,
  MDB_CL_REPLY_BEGIN_SESSION = 0x03,
  MDB_CL_REPLY_SESSION_CANCEL_REQ = 0x04,
  MDB_CL_REPLY_VEND_APPROVED = 0x05,
  MDB_CL_REPLY_VEND_DENIED = 0x06,
  MDB_CL_REPLY_END_SESSION = 0x07,
  MDB_CL_REPLY_CANCELLED = 0x08,
  MDB_CL_REPLY_PERIPHERAL_ID = 0x09,
  MDB_CL_REPLY_MALFUNCTION = 0x0A,
  MDB_CL_REPLY_CMD_OUT_OF_SEQ = 0x0B,
  MDB_CL_REPLY_REVALUE_APPROVED = 0x0C,
  MDB_CL_REPLY_REVALUE_DENIED = 0x0D,
  MDB_CL_REPLY_REVALUE_LIMIT = 0x0E,
  MDB_CL_REPLY_TIME_DATE = 0x0F,
  MDB_CL_REPLY_DIAGNOSTIC = 0xFF,
};

// Short one-byte replies (ACK / NAK / PNAK).
enum : uint8_t {
  MDB_ACK = 0x00,
  MDB_NAK = 0xAA,
  MDB_RET = 0xAA,
  MDB_PNAK = 0xFF,
};

// ---------------------------------------------------------------------------
// High-level semantic event
// ---------------------------------------------------------------------------

// One structured event emitted by the decoder whenever something
// interesting happens on the bus. Events are what drive the triggers,
// sensors, and human-readable log lines.
//
// IMPORTANT: this struct is passed through a FreeRTOS queue with raw
// memcpy semantics, so it MUST be trivially copyable. No std::string,
// no std::vector -- just fixed-size buffers.
struct MdbEvent {
  static constexpr size_t DESC_CAPACITY = 96;

  enum Kind : uint8_t {
    // Cashless lifecycle
    BEGIN_SESSION,
    END_SESSION,
    SESSION_CANCEL_REQUEST,

    // VEND lifecycle (initiated by VMC press-of-a-button)
    VEND_REQUEST,       // user pressed button, VMC is asking cashless to pay
    VEND_APPROVED,      // cashless ok'd the VEND
    VEND_DENIED,        // cashless refused
    VEND_SUCCESS,       // drink dispensed ok; VMC confirms to cashless
    VEND_FAILURE,       // drink NOT dispensed (no water / beans / jam)
    CASH_SALE,          // same as VEND_REQUEST but paid with coins/bills
                        // (VMC informs cashless for statistics)

    // Bill validator
    BV_BILL_ACCEPTED,
    BV_BILL_REJECTED,
    BV_STACKED,
    BV_RESET,
    BV_ERROR,
    BV_STATUS,          // BV enabled/disabled state changed
    BV_BILL_TYPE_CMD,   // host sent BILL TYPE: data32 = (enable<<16)|escrow
    BV_ESCROW_CMD,      // host sent ESCROW: byte_code = 0x01 stack / 0x00 return

    // Derived "sale cycle" events (BV BILL TYPE enable mask transitions).
    // These are the closest proxy for "drink is being prepared" when we
    // only tap the BV <-> host MDB wire (i.e. we don't see the VMC's
    // VEND commands, which go over the Executive bus behind the coin
    // mechanism). Fires on the transition enable != 0 -> enable == 0
    // (host locks the BV for the duration of the sale) and back.
    SALE_CYCLE_BEGIN,   // data32 = enable_mask that was in effect before
    SALE_CYCLE_END,     // data32 = duration_ms of the completed cycle

    // Coin changer
    CHG_COIN_DEPOSITED,
    CHG_COIN_DISPENSED,
    CHG_RESET,
    CHG_ERROR,
    CHG_SLUG,
    CHG_TUBES_UPDATE,

    // Cashless housekeeping
    CL_RESET,
    CL_MALFUNCTION,
    CL_DIAGNOSTIC,
    CL_DISPLAY_REQUEST,

    // Generic raw events (used for the "on_event" catch-all trigger and
    // for debug logging).
    RAW_MASTER_FRAME,
    RAW_SLAVE_FRAME,
  };

  Kind kind{Kind::RAW_MASTER_FRAME};
  uint32_t timestamp_us{0};

  // Per-kind payload. Fields that aren't applicable stay at 0/empty.
  uint32_t item_number{0};     // VEND_*, CASH_SALE
  uint32_t price_cents{0};     // VEND_*, CASH_SALE (already scaled to cents)
  uint32_t funds_cents{0};     // BEGIN_SESSION
  uint32_t data32{0};          // BV_BILL_TYPE_CMD: (enable_mask<<16)|escrow_mask;
                               // SALE_CYCLE_BEGIN: enable_mask (pre-cycle);
                               // SALE_CYCLE_END: duration_ms
  uint8_t byte_code{0};        // BV_ERROR / CHG_ERROR / CL_MALFUNCTION raw code;
                               // also raw BV POLL status byte for BV_BILL_*
  uint8_t bill_type{0};        // BV_BILL_*
  uint8_t coin_type{0};        // CHG_COIN_*
  uint8_t coin_count{0};       // CHG_COIN_DEPOSITED (coins in tube after)
  bool routed_to_stacker{false};
  char description[DESC_CAPACITY]{};  // short human-readable summary
};

// ---------------------------------------------------------------------------
// Scale factors (learned from SETUP responses). Used to convert raw MDB
// units to euro-cents for display / sensors.
// ---------------------------------------------------------------------------
struct MdbScaleFactors {
  // Cashless: price_cents = raw_price * cashless_scale / cashless_divider
  // where cashless_divider = 10^cashless_decimal_places.
  uint16_t cashless_scale{1};
  uint8_t cashless_decimal_places{2};

  // BV: bill value in cents = raw_type_index mapped via bill type list;
  // for the summary, we just remember the scale and decimal places.
  uint16_t bv_scale{1};
  uint8_t bv_decimal_places{2};
  uint16_t bv_bill_value[16]{};  // filled from BV SETUP expansion / BV expansion

  // Changer: coin value in cents per coin type.
  uint16_t changer_scale{1};
  uint8_t changer_decimal_places{2};
  uint16_t changer_coin_value[16]{};
};

// ---------------------------------------------------------------------------
// Name-lookup helpers (pure, no state).
// ---------------------------------------------------------------------------
const char *mdb_address_name(uint8_t base);
const char *mdb_subcommand_name(uint8_t base, uint8_t sub);
const char *mdb_bv_status_name(uint8_t byte);
const char *mdb_changer_status_name(uint8_t byte);
const char *mdb_cashless_reply_name(uint8_t byte);
const char *mdb_vend_subcmd_name(uint8_t byte);
const char *mdb_event_kind_name(MdbEvent::Kind k);

}  // namespace pulse_inspector_mdb
}  // namespace esphome

#endif  // USE_ESP_IDF

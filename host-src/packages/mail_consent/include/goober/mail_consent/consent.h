#pragma once

// Mail consent (design/machine/029.Invites.md §§ 3–6): which addresses a tenant may mail, and the
// proof. Blocks stop every box mail and invite to an address for one tenant, and only a person
// lifts one. Lists need an opt-in, recorded with how and the evidence. Cold outreach is one
// mail per address in 30 days. Every change goes to an append-only log kept five years.
// Addresses are never stored: rows are keyed by address_hash.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "goober/json/json.h"

namespace goober::sqlite {
class Db;
}

namespace goober::mail::consent {

namespace bj = boost::json;

// The mailbox an address reaches, as far as can be known: lowercased, a trailing dot on the
// domain dropped, a +tag dropped, and for gmail.com (and googlemail.com, the same mailboxes) the
// dots in the name dropped. So a block or an unsubscribe can't be stepped around with
// alice+1@ or a.lice@.
std::string canonical_address(std::string_view address);
// The hash every mail ledger keys an address by: of the canonical address, SHA-256, 32 hex digits.
std::string address_hash(std::string_view address);
// The hash before 2026-10-30 (lowercased only), still checked for blocks made then.
std::string legacy_address_hash(std::string_view address);

inline constexpr int64_t kOutreachEveryMs = 30LL * 24 * 3600 * 1000;
inline constexpr int64_t kLogKeepMs = 5LL * 366 * 24 * 3600 * 1000;
// An operator's statement when lifting a block: at least this long, so it says something.
inline constexpr size_t kMinStatement = 20;

struct Block {
  int64_t at_ms = 0;
  std::string how;  // invite_declined, unsubscribed, complaint, operator
  bj::object evidence;
};

struct ListState {
  std::string list;
  std::string state;  // "in" or "out"
  std::string how;    // invite, account, confirmed, operator; or for out: link, account, preferences, operator
  int64_t at_ms = 0;
};

class Store {
 public:
  explicit Store(std::filesystem::path db_file);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // ---- blocks (§ 6)
  std::optional<Block> block(const std::string& tenant, const std::string& hash);
  // A block on the address under either hash.
  std::optional<Block> block_of(const std::string& tenant, std::string_view address);
  // Adds a block; an existing live block is kept as it was. Returns true when one was added.
  bool add_block(const std::string& tenant, const std::string& hash, const std::string& how, const bj::object& evidence,
                 int64_t now_ms);
  // Lifts the live block. `by` is "person" (signed in as the address) or "operator:$who"; the
  // statement is required (an operator's at least kMinStatement). Returns the block lifted, or
  // nullopt with `why` (none to lift, or no statement).
  std::optional<Block> lift(const std::string& tenant, const std::string& hash, const std::string& by,
                            const std::string& statement, const std::string& request_id, int64_t now_ms,
                            std::string* why);

  // ---- lists (§ 4)
  std::optional<ListState> list_state(const std::string& tenant, const std::string& hash, const std::string& list);
  std::vector<ListState> lists(const std::string& tenant, const std::string& hash);
  // Records an opt-in. Refused for a blocked address, and for how "operator" when the person
  // opted out of the list: an operator never re-subscribes someone who said no.
  bool opt_in(const std::string& tenant, const std::string& hash, const std::string& list, const std::string& how,
              const bj::object& evidence, int64_t now_ms, std::string* why);
  void opt_out(const std::string& tenant, const std::string& hash, const std::string& list, const std::string& how,
               const bj::object& evidence, int64_t now_ms);
  int64_t count_in(const std::string& tenant, const std::string& list);
  // The hashes in a list, for sending a list's mail to accounts the host can match.
  std::vector<std::string> members(const std::string& tenant, const std::string& list);

  // ---- outreach (§ 3)
  struct Outreach {
    int64_t first_ms = 0, last_ms = 0, count = 0;
    bool opted_out = false;
  };
  std::optional<Outreach> outreach(const std::string& tenant, const std::string& hash);
  void outreach_opt_out(const std::string& tenant, const std::string& hash, const std::string& how, int64_t now_ms);
  void outreach_sent(const std::string& tenant, const std::string& hash, int64_t now_ms);
  int64_t outreach_since(const std::string& tenant, int64_t since_ms);

  // ---- confirmations: list-confirm mails and confirm-category mails asked for (§§ 3, 4)
  // How many of `kind` went to the address since `since_ms`; and recording one.
  int64_t asked_since(const std::string& tenant, const std::string& hash, const std::string& kind, int64_t since_ms);
  void asked(const std::string& tenant, const std::string& hash, const std::string& kind, int64_t now_ms);

  // ---- the record
  // Every change for one address, oldest first: {at_ms, what, list, how, evidence}.
  bj::array history(const std::string& tenant, const std::string& hash);
  // Drops log rows past kLogKeepMs, and asked rows past 30 days.
  void prune(int64_t now_ms);
  // Moves the old unsubscribed.db's rows into blocks (how "unsubscribed"), once; the file is
  // renamed to unsubscribed.db.imported. Returns how many.
  int import_unsubscribed(const std::filesystem::path& file, int64_t now_ms);

 private:
  void log(const std::string& tenant, const std::string& hash, const std::string& what, const std::string& list,
           const std::string& how, const bj::object& evidence, int64_t now_ms);
  std::mutex mu_;
  std::unique_ptr<sqlite::Db> db_;
};

}  // namespace goober::mail::consent

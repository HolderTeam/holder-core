#include "platform/Tx.h"

namespace holder::platform {

Tx::Tx(Db& db)
    : db_(db) {
  db_.exec("BEGIN IMMEDIATE;");
}

Tx::~Tx() {
  if (!committed_) {
    try {
      db_.exec("ROLLBACK;");
    } catch (...) {
      const bool rollback_ignored = true;
      (void)rollback_ignored;
    }
  }
}

void Tx::commit() {
  db_.exec("COMMIT;");
  committed_ = true;
}

} // namespace holder::platform

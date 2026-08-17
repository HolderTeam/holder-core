#pragma once
#include "platform/Db.h"

namespace holder::platform {

class Tx {
 public:
  explicit Tx(Db& db);
  ~Tx();

  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;

  void commit();

 private:
  Db& db_;
  bool committed_ = false;
};

} // namespace holder::platform

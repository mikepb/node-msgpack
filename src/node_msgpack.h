#pragma once

#ifndef _NODE_MSGPACK_H_
#define _NODE_MSGPACK_H_

#include <exception>

namespace node_msgpack {

#define MSGPACK_FLAGS_NONE              0x0000
#define MSGPACK_NO_TOJSON               0x0001
#define MSGPACK_FUNCTION_TO_STRING      0x0002
#define MSGPACK_REGEXP_TO_STRING        0x0004
#define MSGPACK_DATE_TO_DOUBLE          0x0008

class msgpack_error : public std::exception {
public:
  msgpack_error(const char *msg)
  : std::exception(), msg_(msg) {}
  virtual const char *what() const throw() {
    return msg_;
  }
private:
  const char *msg_;
};

class circular_structure : public msgpack_error {
public:
  circular_structure(const char *msg = "circular_structure")
  : msgpack_error(msg) {}
};

class type_error : public msgpack_error {
public:
  type_error(const char *msg = "type_error")
  : msgpack_error(msg) {}
};

class bad_data : public msgpack_error {
public:
  bad_data(const char *msg = "bad_data")
  : msgpack_error(msg) {}
};

} // namespace node_msgpack

#include "node_msgpack/pack.h"
#include "node_msgpack/unpack.h"

#endif

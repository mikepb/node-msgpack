#pragma once

#ifndef _NODE_MSGPACK_H_
#define _NODE_MSGPACK_H_

#include <exception>

#define NODE_MSGPACK_FN_TOSTRING    0x0001
#define NODE_MSGPACK_RE_TOSTRING    0x0002
#define NODE_MSGPACK_CALL_TOJSON    0x0004
#define NODE_MSGPACK_DATE_DOUBLE    0x0008

namespace node_msgpack {

class circular_structure : public std::exception {
public:
  virtual const char *what() const throw() {
    return "circular_structure";
  }
};

class type_error : public std::exception {
public:
  virtual const char *what() const throw() {
    return "type_error";
  }
};

class msgpack_error : public std::exception {
public:
  msgpack_error(const char *msg) : msg_(msg) {}
  virtual const char *what() const throw() {
    return msg_;
  }
private:
  const char *msg_;
};

class bad_data : public msgpack_error {
public:
  bad_data() : msgpack_error("bad_data") {}
};

} // namespace node_msgpack

#include "node_msgpack/pack.h"
#include "node_msgpack/unpack.h"

#endif

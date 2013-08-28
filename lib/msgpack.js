/**
 * Module dependencies.
 */

var buffer = require('buffer')
  , EventEmitter = require('events').EventEmitter
  , binding = require("../build/Release/msgpackBinding")
  , sys;

try {
  sys = require('util');
} catch (e) {
  sys = require('sys');
}

/**
 * Accessor for bytes read when unpacking.
 */

Object.defineProperty(exports, 'offset', {
  get: function() { return binding.offset; }
});

/**
 * Pack JavaScript to MessagePack data.
 *
 * @param {Mixed} args...
 * @return {Buffer}
 * @api public
 */

var pack = exports.pack = function() {
  var args = [].slice.call(arguments), arg;
  for (var i = 0; i < args.length; i++) {
    arg = args[i];
    if (arg && typeof arg === 'object' && typeof arg.toJSON === 'function')
      args[i] = arg.toJSON();
  }
  args.unshift(null);
  return binding.pack.apply(null, args);
}

/**
 * Unpack MessagePack data to JavaScript.
 *
 * @param {Buffer} buf
 * @return {Mixed}
 * @api public
 */

var unpack = exports.unpack = function(buf) {
  // optimization: if the entire buffer has been read,
  // allow the binding to skip setting the offset
  binding.offset = buf.length;
  return binding.unpack(buf);
}

/**
 * Initialize a new `Stream` with the given `stream`.
 *
 * @param {Stream} stream
 * @api public
 */

var Stream = exports.Stream = function Stream(stream) {
  EventEmitter.call(this);
  this.stream = stream;
  stream.addListener('data', this._onData.bind(this));
}

/**
 * Send a message down the stream.
 *
 * Allows the caller to pass additional arguments, which are passed
 * faithfully down to the write() method of the underlying stream.
 */

Stream.prototype.send = function() {
  var args = [].slice.call(arguments);
  return this.stream.write.apply(s, args);
};

/**
 * Emits JavaScript objects from the underlying stream.
 *
 * @param {Buffer} buf
 * @api private
 */

Stream.prototype._onData = function(buf) {
  // Make sure that self.buf reflects the entirety of the unread stream
  // of bytes; it needs to be a single buffer
  if (this.buf) {
    // create new buffer of sufficient length
    var b = new buffer.Buffer(this.buf.length + buf.length);
    // copy data into continuous buffer
    this.buf.copy(b, 0, 0, this.buf.length);
    buf.copy(b, this.buf.length, 0, buf.length);
    // replace old buffer
    this.buf = b;
  } else {
    this.buf = buf;
  }

  // Consume messages from the stream, one by one
  while (this.buf && this.buf.length > 0) {
    // unpack
    var msg = unpack(this.buf);
    if (!msg) break;
    // emit chunk
    this.emit('msg', msg);
    // trim buffer
    this.buf =
      (0 < binding.offset && binding.offset < this.buf.length)
      ? this.buf.slice(binding.offset, this.buf.length)
      : null;
  }
};

sys.inherits(Stream, EventEmitter);

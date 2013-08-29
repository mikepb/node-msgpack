crypto = require "crypto"
expect = require "expect.js"
{ pack, unpack } = msgpack = require "../lib/msgpack"

describe "binding", ->
  describe "unpack", ->
    it "should unpack nothing", ->
      expect(unpack []).to.be undefined

    it "should unpack nil", ->
      expect(unpack [0xc0]).to.be null

    it "should unpack false", ->
      expect(unpack [0xc2]).to.be false

    it "should unpack true", ->
      expect(unpack [0xc3]).to.be true

    it "should unpack float 32", ->
      expect(unpack [0xca, 0x00, 0x00, 0x00, 0x00]).to.be 0

    it "should unpack float 64", ->
      expect(unpack [0xca, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]).to.be 0

    it "should unpack positive fixint", ->
      @slow 100
      expect(unpack [i]).to.be i for i in [0..0x0f] by 3
      expect(unpack [i]).to.be i for i in [0x10..0x7f] by 3
      expect(unpack [0x7f]).to.be 0x7f

    it "should unpack uint 8", ->
      @slow 100
      expect(unpack [0xcc, i]).to.be i for i in [0..0x0f]
      expect(unpack [0xcc, i]).to.be i for i in [0x10..0xff] by 5
      expect(unpack [0xcc, 0xff]).to.be 0xff

    it "should unpack uint 16", ->
      @slow 100
      expect(unpack [0xcd, 0, i]).to.be i for i in [0..0x0f]
      expect(unpack [0xcd, 0, i]).to.be i for i in [0x10..0xff] by 11
      expect(unpack [0xcd, 0, 0xff]).to.be 0xff
      expect(unpack [0xcd, i, 0]).to.be i << 8 for i in [0..0xff] by 11
      expect(unpack [0xcd, 0xff, 0]).to.be 0xff00

    it "should unpack uint 32", ->
      @slow 100
      expect(unpack [0xce, 0, 0, 0, i]).to.be i for i in [0..0x0f]
      expect(unpack [0xce, 0, 0, 0, i]).to.be i for i in [0x10..0xff] by 13
      expect(unpack [0xce, 0, 0, 0, 0xff]).to.be 0xff
      expect(unpack [0xce, 0, 0, i, 0]).to.be i << 8 for i in [0..0xff] by 13
      expect(unpack [0xce, 0, 0, 0xff, 0]).to.be 0xff00
      expect(unpack [0xce, 0, i, 0, 0]).to.be i << 16 for i in [0..0xff] by 13
      expect(unpack [0xce, 0, 0xff, 0, 0]).to.be 0xff0000
      expect(unpack [0xce, i, 0, 0, 0]).to.be i << 24 for i in [0..0x7f] by 13
      expect(unpack [0xce, 0x7f, 0, 0, 0]).to.be 0x7f000000
      expect(unpack [0xce, 0x80, 0, 0, 0]).to.be 0x80000000
      expect(unpack [0xce, 0xff, 0, 0, 0]).to.be 0xff000000
      expect(unpack [0xce, 0xff, 0, 0xff, 0]).to.be 0xff00ff00
      expect(unpack [0xce, 0xff, 0, 0, 0xff]).to.be 0xff0000ff
      expect(unpack [0xce, 0xff, 0xff, 0xff, 0xff]).to.be 0xffffffff

    it "should unpack uint 64", ->
      @slow 200
      expect(unpack [0xcf, 0, 0, 0, 0, 0, 0, 0, 0]).to.be 0
      expect(unpack [0xcf, 0, 0, 0, 0, 0x80, 0, 0, 0]).to.be 0x80000000
      expect(unpack [0xcf, 0, 0, 0, 0, 0xff, 0, 0, 0]).to.be 0xff000000
      expect(unpack [0xcf, 0, 0, 0, 0, 0xff, 0, 0xff, 0]).to.be 0xff00ff00
      expect(unpack [0xcf, 0, 0, 0, 0, 0xff, 0, 0, 0xff]).to.be 0xff0000ff
      expect(unpack [0xcf, 0, 0, 0, i, 0, 0, 0, 0]).to.be i * 0x0100000000 for i in [0...0xff] by 17
      expect(unpack [0xcf, 0, 0, i, 0, 0, 0, 0, 0]).to.be i * 0x010000000000 for i in [0...0xff] by 17
      expect(unpack [0xcf, 0, i, 0, 0, 0, 0, 0, 0]).to.be i * 0x01000000000000 for i in [0...0xff] by 17
      expect(unpack [0xcf, i, 0, 0, 0, 0, 0, 0, 0]).to.be i * 0x0100000000000000 for i in [0...0xff] by 17
      expect(unpack [0xcf, 0x7f, 0, 0, 0, 0, 0, 0, 0]).to.be 0x7f00000000000000
      expect(unpack [0xcf, 0x80, 0, 0, 0, 0, 0, 0, 0]).to.be 0x8000000000000000
      expect(unpack [0xcf, 0xff, 0, 0, 0, 0, 0, 0, 0]).to.be 0xff00000000000000
      expect(unpack [0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]).to.be 0xffffffffffffffff

    it "should unpack negative fixing", ->
      @slow 100
      expect(unpack [i]).to.be -((~i & 0xff) + 1) for i in [0xe0..0xff]
      expect(unpack [0xff]).to.be -1

    it "should unpack uint 8", ->
      @slow 100
      expect(unpack [0xd0, i]).to.be i for i in [0..0x0f]
      expect(unpack [0xd0, i]).to.be i for i in [0x10..0x7f] by 7
      expect(unpack [0xd0, 0x7f]).to.be 0x7f
      expect(unpack [0xd0, i]).to.be -((~i & 0xff) + 1) for i in [0x80..0x8f]
      expect(unpack [0xd0, i]).to.be -((~i & 0xff) + 1) for i in [0x90..0xff] by 7
      expect(unpack [0xd0, 0xff]).to.be -1

    it "should unpack uint 16", ->
      @slow 200
      expect(unpack [0xd1, 0, i]).to.be i for i in [0..0xf]
      expect(unpack [0xd1, 0, i]).to.be i for i in [0x10..0xff] by 11
      expect(unpack [0xd1, 0, 0xff]).to.be 0xff
      expect(unpack [0xd1, i, 0]).to.be i << 8 for i in [0..0x7f] by 11
      expect(unpack [0xd1, 0x7f, 0]).to.be 0x7f00
      expect(unpack [0xd1, i, 0]).to.be -((~(i * 0x100) & 0xffff) + 1) for i in [0x80..0x8f]
      expect(unpack [0xd1, i, 0]).to.be -((~(i * 0x100) & 0xffff) + 1) for i in [0x90..0xff] by 11
      expect(unpack [0xd1, 0xff, i]).to.be -((~i & 0xff) + 1) for i in [0x00..0xef] by 11
      expect(unpack [0xd1, 0xff, i]).to.be -((~i & 0xff) + 1) for i in [0xf0..0xff]
      expect(unpack [0xd1, 0xff, 0xff]).to.be -1

    it "should unpack uint 32", ->
      @slow 200
      expect(unpack [0xd2, 0, 0, 0, i]).to.be i for i in [0..0x08]
      expect(unpack [0xd2, 0, 0, 0, i]).to.be i for i in [0x09..0xff] by 17
      expect(unpack [0xd2, 0, 0, 0, 0xff]).to.be 0xff
      expect(unpack [0xd2, 0, 0, i, 0]).to.be i << 8 for i in [0..0xff] by 17
      expect(unpack [0xd2, 0, 0, 0xff, 0]).to.be 0xff00
      expect(unpack [0xd2, 0, i, 0, 0]).to.be i << 16 for i in [0..0xff] by 17
      expect(unpack [0xd2, 0, 0xff, 0, 0]).to.be 0xff0000
      expect(unpack [0xd2, i, 0, 0, 0]).to.be i << 24 for i in [0..0x7f] by 17
      expect(unpack [0xd2, 0x7f, 0, 0, 0]).to.be 0x7f000000
      expect(unpack [0xd2, 0x80, 0, 0, 0]).to.be -0x80000000
      expect(unpack [0xd2, 0x81, 0, 0, 0]).to.be -0x7f000000
      expect(unpack [0xd2, 0xff, 0, 0, 0]).to.be -0x01000000
      expect(unpack [0xd2, 0xff, 0, 0xff, 0]).to.be -0xff0100
      expect(unpack [0xd2, 0xff, 0, 0, 0xff]).to.be -0xffff01
      expect(unpack [0xd2, 0xff, 0xff, 0xff, 0xff]).to.be -1

    it "should unpack uint 64", ->
      @slow 200
      expect(unpack [0xd3, 0, 0, 0, 0, 0, 0, 0, 0]).to.be 0
      expect(unpack [0xd3, 0, 0, 0, 0, 0x80, 0, 0, 0]).to.be 0x80000000
      expect(unpack [0xd3, 0, 0, 0, 0, 0xff, 0, 0, 0]).to.be 0xff000000
      expect(unpack [0xd3, 0, 0, 0, 0, 0xff, 0, 0xff, 0]).to.be 0xff00ff00
      expect(unpack [0xd3, 0, 0, 0, 0, 0xff, 0, 0, 0xff]).to.be 0xff0000ff
      expect(unpack [0xd3, 0, 0, 0, i, 0, 0, 0, 0]).to.be i * 0x0100000000 for i in [0...0xff] by 17
      expect(unpack [0xd3, 0, 0, i, 0, 0, 0, 0, 0]).to.be i * 0x010000000000 for i in [0...0xff] by 17
      expect(unpack [0xd3, 0, i, 0, 0, 0, 0, 0, 0]).to.be i * 0x01000000000000 for i in [0...0xff] by 17
      expect(unpack [0xd3, i, 0, 0, 0, 0, 0, 0, 0]).to.be i * 0x0100000000000000 for i in [0...0x7f] by 17
      expect(unpack [0xd3, 0x7f, 0, 0, 0, 0, 0, 0, 0]).to.be 0x7f00000000000000
      expect(unpack [0xd3, 0x80, 0, 0, 0, 0, 0, 0, 0]).to.be -0x8000000000000000
      expect(unpack [0xd3, 0xff, 0, 0, 0, 0, 0, 0, 0]).to.be -0x0100000000000000
      expect(unpack [0xd3, 0xff, 0xff, 0, 0, 0, 0, 0, 0]).to.be -0x1000000000000
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0]).to.be -0x10000000000
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0]).to.be -0x100000000
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0]).to.be -0x1000000
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0]).to.be -0x10000
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0]).to.be -0x100
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, i]).to.be -((~i & 0xff) + 1) for i in [0x00..0xef] by 11
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, i]).to.be -((~i & 0xff) + 1) for i in [0xf0..0xff]
      expect(unpack [0xd3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]).to.be -1

    it "should unpack fixstr", ->
      s = "\0abcdefghijklmnopqrstuvwxyz01234"
      b = new Buffer s
      for i in [0...31]
        b[0] = 0xa0 + i
        expect(unpack b).to.be s.substr(1, i)
      expect(unpack [0xa3, 0xe2, 0x88, 0x9e]).to.be "∞"

    it "should unpack str 8", ->
      @slow 200
      s = crypto.randomBytes(0x90).toString("hex")
      b = new Buffer s
      b[0] = 0xd9
      b[1] = 0
      for i in [0..0x0f]
        b[1] = i
        expect(unpack b).to.be s.substr(2, i)
      for i in [0x10..0xff] by 3
        b[1] = i
        expect(unpack b).to.be s.substr(2, i)
      expect(unpack [0xd9, 0x03, 0xe2, 0x88, 0x9e]).to.be "∞"

    it "should unpack str 16", ->
      @slow 500
      s = crypto.randomBytes(0x9001).toString("hex")
      b = new Buffer s
      b[0] = 0xda
      b[1] = 0
      b[2] = 0
      for i in [0..0x0f]
        b[2] = i
        expect(unpack b).to.be s.substr(3, i)
      for i in [0x10..0xfffc] by 397
        b[1] = (i & 0xff00) >> 8
        b[2] = i & 0xff
        expect(unpack b).to.be s.substr(3, i)
      for i in [0xfd..0xff]
        b[2] = i
        expect(unpack b).to.be s.substr(3, 0xff00 + i)
      expect(unpack [0xda, 0, 0x03, 0xe2, 0x88, 0x9e]).to.be "∞"

    it "should unpack str 32", ->
      @slow 500
      # only test up to 1 MB
      s = crypto.randomBytes(0x90001).toString("hex")
      b = new Buffer s
      b[0] = 0xdb
      b[1] = 0
      b[2] = 0
      b[3] = 0
      for i in [0..0x0f]
        b[4] = i
        expect(unpack b).to.be s.substr(5, i)
      for i in [0x10..0xffffc] by 12379
        b[2] = (i & 0xff0000) >> 16
        b[3] = (i & 0xff00) >> 8
        b[4] = i & 0xff
        expect(unpack b).to.be s.substr(5, i)
      b[2] = 0x0f
      b[3] = 0xff
      for i in [0xfd..0xff]
        b[4] = i
        expect(unpack b).to.be s.substr(5, 0xfff00 + i)
      expect(unpack [0xdb, 0, 0, 0, 0x03, 0xe2, 0x88, 0x9e]).to.be "∞"

    it "should unpack fixarray (null)", ->
      t = (null for i in [0..15])
      b = new Buffer(0xc0 for i in [0..16])
      for i in [0..15]
        b[0] = 0x90 + i
        expect(unpack b).to.eql t.slice(0, i)

    it "should unpack fixarray (fixint)", ->
      t = (i for i in [1..16])
      b = new Buffer(i for i in [0..16])
      for i in [0..15]
        b[0] = 0x90 + i
        expect(unpack b).to.eql t.slice(0, i)

    it "should unpack fixarray (fixmap)", ->
      t = ({} for i in [0..15])
      b = new Buffer(0x80 for i in [0..16])
      for i in [0..15]
        b[0] = 0x90 + i
        expect(unpack b).to.eql t.slice(0, i)

    # case 0xdc: // array 16
    #   l = r.u16();
    #   v = Array::New(l);
    #   break;

    # case 0xdd: // array 32
    #   l = r.u32();
    #   v = Array::New(l);
    #   break;

    # case 0x80 ... 0x8f: // fixmap
    #   l = r.ut(0x0f);
    #   v = Object::New();
    #   break;

    # case 0xde: // map 16
    #   l = r.u16();
    #   v = Object::New();
    #   break;

    # case 0xdf: // map 32
    #   l = r.u32();
    #   v = Object::New();
    #   break;

    # case 0xc4: // bin 8
    # case 0xc5: // bin 16
    # case 0xc6: // bin 32
    # case 0xc7: // ext 8
    # case 0xc8: // ext 16
    # case 0xc9: // ext 32

    # case 0xd4: // fixext 1
    # case 0xd5: // fixext 2
    # case 0xd6: // fixext 4
    # case 0xd7: // fixext 8
    # case 0xd8: // fixext 16

    # default:

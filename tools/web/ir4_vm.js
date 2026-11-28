/* ir4_vm.js - 32-bit IR4th Bytecode VM & Web Runtime */
(function (root, cliArgs) {
  "use strict";

  var OP_RET = 0;
  var OP_ENTER = 1;
  var OP_LEAVE = 2;
  var OP_PUSH_0_32 = 6;
  var OP_PUSH_I8 = 7;
  var OP_PUSH_W = 8;
  var OP_PUSH_I32 = 9;
  var OP_PUSH_F = 10;
  var OP_PUSH_DATA = 11;
  var OP_PUSH_FN = 12;
  var OP_INC = 13;
  var OP_DEC = 14;
  var OP_ADD_IMM8 = 15;
  var OP_STATIC_GET = 16;
  var OP_STATIC_GET32 = 17;
  var OP_STATIC_SET = 18;
  var OP_STATIC_SET32 = 19;
  var OP_LOCAL_GET = 20;
  var OP_LOCAL_GET32 = 21;
  var OP_LOCAL_SET = 22;
  var OP_LOCAL_SET32 = 23;
  var OP_LOCAL_ADDR = 24;
  var OP_DUP = 25;
  var OP_DUP32 = 26;
  var OP_DROP = 27;
  var OP_DROP32 = 28;
  var OP_SWAP = 29;
  var OP_SWAP_W32 = 30;
  var OP_SWAP_32W = 31;
  var OP_SWAP32 = 32;
  var OP_JMP = 33;
  var OP_JZ = 34;
  var OP_JTAB = 35;
  var OP_CALL = 36;
  var OP_ICALL = 37;
  var OP_CALL_EXTERN = 38;
  var OP_ADD = 39;
  var OP_SUB = 40;
  var OP_MUL = 41;
  var OP_DIV = 42;
  var OP_MOD = 43;
  var OP_UDIV = 44;
  var OP_UMOD = 45;
  var OP_AND = 46;
  var OP_OR = 47;
  var OP_XOR = 48;
  var OP_INVERT = 49;
  var OP_LSHIFT = 50;
  var OP_ARSHIFT = 51;
  var OP_RSHIFT = 52;
  var OP_FADD = 53;
  var OP_FSUB = 54;
  var OP_FMUL = 55;
  var OP_FDIV = 56;
  var OP_FNEG = 57;
  var OP_ADD32 = 58;
  var OP_SUB32 = 59;
  var OP_MUL32 = 60;
  var OP_DIV32 = 61;
  var OP_MOD32 = 62;
  var OP_UDIV32 = 63;
  var OP_UMOD32 = 64;
  var OP_AND32 = 65;
  var OP_OR32 = 66;
  var OP_XOR32 = 67;
  var OP_INV32 = 68;
  var OP_SHL32 = 69;
  var OP_ASHR32 = 70;
  var OP_RSHR32 = 71;
  var OP_EQ = 72;
  var OP_NE = 73;
  var OP_LT = 74;
  var OP_GT = 75;
  var OP_LE = 76;
  var OP_GE = 77;
  var OP_ULT = 78;
  var OP_UGT = 79;
  var OP_ULE = 80;
  var OP_UGE = 81;
  var OP_FEQ = 82;
  var OP_FNE = 83;
  var OP_FLT = 84;
  var OP_FGT = 85;
  var OP_EQ32 = 86;
  var OP_NE32 = 87;
  var OP_LT32 = 88;
  var OP_GT32 = 89;
  var OP_LE32 = 90;
  var OP_GE32 = 91;
  var OP_ULT32 = 92;
  var OP_UGT32 = 93;
  var OP_ULE32 = 94;
  var OP_UGE32 = 95;
  var OP_I8FETCH = 96;
  var OP_U8FETCH = 97;
  var OP_I8STORE = 98;
  var OP_FETCH = 99;
  var OP_STORE = 100;
  var OP_FETCH32 = 101;
  var OP_STORE32 = 102;
  var OP_BCOPY = 103;
  var OP_SEXT = 104;
  var OP_W2F = 105;
  var OP_F2W = 106;
  var OP_U16FETCH = 110;
  var OP_I16FETCH = 111;
  var OP_STORE16 = 112;

  function parseIr4Bin(bytes) {
    var dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (bytes.length < 28 ||
        bytes[0] !== 0x49 || bytes[1] !== 0x52 || bytes[2] !== 0x34 || bytes[3] !== 0x42) {
      throw new Error("Invalid IR4B binary");
    }
    var base = dv.getUint32(4, true);
    var entry = dv.getUint32(8, true);
    var codeSize = dv.getUint32(12, true);
    var initSize = dv.getUint32(16, true);
    var bssSize = dv.getUint32(20, true);
    var extCount = dv.getUint32(24, true);
    var pos = 28;
    var externs = [];
    for (var i = 0; i < extCount; i++) {
      var s = "";
      while (pos < bytes.length && bytes[pos] !== 0) {
        s += String.fromCharCode(bytes[pos++]);
      }
      pos++;
      externs.push(s);
    }
    var code = bytes.subarray(pos, pos + codeSize);
    pos += codeSize;
    var initData = bytes.subarray(pos, pos + initSize);
    return {
      base: base,
      entry: entry,
      codeSize: codeSize,
      initSize: initSize,
      bssSize: bssSize,
      externs: externs,
      code: code,
      initData: initData
    };
  }

  function createIr4VM(binBytes, host, memSize) {
    var img = parseIr4Bin(binBytes);
    var totalMem = memSize || (1024 * 1024);
    var buf = new ArrayBuffer(totalMem);
    var mem = new Uint8Array(buf);
    var dv = new DataView(buf);

    mem.set(img.code, img.base);
    mem.set(img.initData, img.base + img.codeSize);

    var spTop = totalMem - 0x1000;
    var sp = spTop;
    var fp = totalMem - 0x20000;
    var pc = img.entry;
    var callStack = [0xFFFFFFFF];
    var halted = false;
    var exitCode = 0;
    var yieldRequested = false;

    var vm = {
      mem: mem,
      dv: dv,
      img: img,
      halted: false,
      exitCode: 0,
      popI32: function () {
        var v = dv.getInt32(sp, true);
        sp += 4;
        return v;
      },
      popU32: function () {
        var v = dv.getUint32(sp, true);
        sp += 4;
        return v;
      },
      pushI32: function (v) {
        sp -= 4;
        dv.setInt32(sp, v | 0, true);
      },
      pushU32: function (v) {
        sp -= 4;
        dv.setUint32(sp, v >>> 0, true);
      },
      readStr: function (addr) {
        var s = "";
        while (addr < totalMem && mem[addr] !== 0) {
          s += String.fromCharCode(mem[addr++]);
        }
        return s;
      },
      yieldNow: function () {
        yieldRequested = true;
      },
      retryExtern: function () {
        pc -= 3;
        yieldRequested = true;
      },
      halt: function (code) {
        halted = true;
        vm.halted = true;
        exitCode = code | 0;
        vm.exitCode = exitCode;
        yieldRequested = true;
      },
      step: function (budget) {
        if (halted) return "halted";
        yieldRequested = false;
        var steps = budget || 100000;
        while (steps-- > 0) {
          var op = mem[pc++];
          if (op & 0x80) {
            sp -= 4;
            dv.setInt32(sp, op & 0x7F, true);
            continue;
          }
          var a, b, addr, off, v, target;
          switch (op) {
            case OP_RET:
              pc = callStack.pop();
              if (pc === 0xFFFFFFFF) {
                halted = true;
                vm.halted = true;
                exitCode = (sp < spTop) ? dv.getInt32(sp, true) : 0;
                vm.exitCode = exitCode;
                return "halted";
              }
              break;
            case OP_ENTER:
              fp -= dv.getUint32(pc, true);
              pc += 4;
              break;
            case OP_LEAVE:
              fp += dv.getUint32(pc, true);
              pc += 4;
              break;
            case OP_PUSH_0_32:
              sp -= 4;
              dv.setInt32(sp, 0, true);
              break;
            case OP_PUSH_I8:
              v = dv.getInt8(pc++);
              sp -= 4;
              dv.setInt32(sp, v, true);
              break;
            case OP_PUSH_W:
            case OP_PUSH_I32:
            case OP_PUSH_F:
            case OP_PUSH_DATA:
            case OP_PUSH_FN:
              v = dv.getInt32(pc, true);
              pc += 4;
              sp -= 4;
              dv.setInt32(sp, v, true);
              break;
            case OP_INC:
              dv.setInt32(sp, (dv.getInt32(sp, true) + 1) | 0, true);
              break;
            case OP_DEC:
              dv.setInt32(sp, (dv.getInt32(sp, true) - 1) | 0, true);
              break;
            case OP_ADD_IMM8:
              v = dv.getInt8(pc++);
              dv.setInt32(sp, (dv.getInt32(sp, true) + v) | 0, true);
              break;
            case OP_STATIC_GET:
            case OP_STATIC_GET32:
              addr = dv.getUint32(pc, true);
              pc += 4;
              sp -= 4;
              dv.setInt32(sp, dv.getInt32(addr, true), true);
              break;
            case OP_STATIC_SET:
            case OP_STATIC_SET32:
              addr = dv.getUint32(pc, true);
              pc += 4;
              dv.setInt32(addr, dv.getInt32(sp, true), true);
              sp += 4;
              break;
            case OP_LOCAL_GET:
            case OP_LOCAL_GET32:
              off = dv.getUint32(pc, true);
              pc += 4;
              sp -= 4;
              dv.setInt32(sp, dv.getInt32(fp + off, true), true);
              break;
            case OP_LOCAL_SET:
            case OP_LOCAL_SET32:
              off = dv.getUint32(pc, true);
              pc += 4;
              dv.setInt32(fp + off, dv.getInt32(sp, true), true);
              sp += 4;
              break;
            case OP_LOCAL_ADDR:
              off = dv.getUint32(pc, true);
              pc += 4;
              sp -= 4;
              dv.setUint32(sp, fp + off, true);
              break;
            case OP_DUP:
            case OP_DUP32:
              v = dv.getInt32(sp, true);
              sp -= 4;
              dv.setInt32(sp, v, true);
              break;
            case OP_DROP:
            case OP_DROP32:
              sp += 4;
              break;
            case OP_SWAP:
            case OP_SWAP_W32:
            case OP_SWAP_32W:
            case OP_SWAP32:
              a = dv.getInt32(sp, true);
              b = dv.getInt32(sp + 4, true);
              dv.setInt32(sp, b, true);
              dv.setInt32(sp + 4, a, true);
              break;
            case OP_JMP:
              pc = dv.getUint32(pc, true);
              break;
            case OP_JZ:
              target = dv.getUint32(pc, true);
              pc += 4;
              v = dv.getInt32(sp, true);
              sp += 4;
              if (v === 0) pc = target;
              break;
            case OP_JTAB:
              v = dv.getUint32(sp, true);
              sp += 4;
              pc = dv.getUint32(pc + v * 4, true);
              break;
            case OP_CALL:
              target = dv.getUint32(pc, true);
              callStack.push(pc + 4);
              pc = target;
              break;
            case OP_ICALL:
              target = dv.getUint32(sp, true);
              sp += 4;
              callStack.push(pc);
              pc = target;
              break;
            case OP_CALL_EXTERN: {
              var id = dv.getUint16(pc, true);
              pc += 2;
              var name = img.externs[id];
              var fn = host && host.externs && host.externs[name];
              if (!fn) {
                throw new Error("Unsupported extern: " + name);
              }
              fn(vm);
              if (yieldRequested) return "yield";
              break;
            }
            case OP_ADD:
            case OP_ADD32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, (dv.getInt32(sp, true) + b) | 0, true);
              break;
            case OP_SUB:
            case OP_SUB32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, (dv.getInt32(sp, true) - b) | 0, true);
              break;
            case OP_MUL:
            case OP_MUL32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, Math.imul(dv.getInt32(sp, true), b), true);
              break;
            case OP_DIV:
            case OP_DIV32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, (dv.getInt32(sp, true) / b) | 0, true);
              break;
            case OP_MOD:
            case OP_MOD32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, (dv.getInt32(sp, true) % b) | 0, true);
              break;
            case OP_UDIV:
            case OP_UDIV32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setUint32(sp, (dv.getUint32(sp, true) / b) >>> 0, true);
              break;
            case OP_UMOD:
            case OP_UMOD32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setUint32(sp, (dv.getUint32(sp, true) % b) >>> 0, true);
              break;
            case OP_AND:
            case OP_AND32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) & b, true);
              break;
            case OP_OR:
            case OP_OR32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) | b, true);
              break;
            case OP_XOR:
            case OP_XOR32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) ^ b, true);
              break;
            case OP_INVERT:
            case OP_INV32:
              dv.setInt32(sp, ~dv.getInt32(sp, true), true);
              break;
            case OP_LSHIFT:
            case OP_SHL32:
              b = dv.getUint32(sp, true); sp += 4;
              a = dv.getInt32(sp, true);
              dv.setInt32(sp, b >= 32 ? 0 : (a << b), true);
              break;
            case OP_ARSHIFT:
            case OP_ASHR32:
              b = dv.getUint32(sp, true); sp += 4;
              a = dv.getInt32(sp, true);
              dv.setInt32(sp, b >= 32 ? (a >> 31) : (a >> b), true);
              break;
            case OP_RSHIFT:
            case OP_RSHR32:
              b = dv.getUint32(sp, true); sp += 4;
              a = dv.getUint32(sp, true);
              dv.setUint32(sp, b >= 32 ? 0 : (a >>> b), true);
              break;
            case OP_FADD:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setFloat32(sp, dv.getFloat32(sp, true) + b, true);
              break;
            case OP_FSUB:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setFloat32(sp, dv.getFloat32(sp, true) - b, true);
              break;
            case OP_FMUL:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setFloat32(sp, dv.getFloat32(sp, true) * b, true);
              break;
            case OP_FDIV:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setFloat32(sp, dv.getFloat32(sp, true) / b, true);
              break;
            case OP_FNEG:
              dv.setFloat32(sp, -dv.getFloat32(sp, true), true);
              break;
            case OP_EQ:
            case OP_EQ32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) === b ? -1 : 0, true);
              break;
            case OP_NE:
            case OP_NE32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) !== b ? -1 : 0, true);
              break;
            case OP_LT:
            case OP_LT32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) < b ? -1 : 0, true);
              break;
            case OP_GT:
            case OP_GT32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) > b ? -1 : 0, true);
              break;
            case OP_LE:
            case OP_LE32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) <= b ? -1 : 0, true);
              break;
            case OP_GE:
            case OP_GE32:
              b = dv.getInt32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getInt32(sp, true) >= b ? -1 : 0, true);
              break;
            case OP_ULT:
            case OP_ULT32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getUint32(sp, true) < b ? -1 : 0, true);
              break;
            case OP_UGT:
            case OP_UGT32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getUint32(sp, true) > b ? -1 : 0, true);
              break;
            case OP_ULE:
            case OP_ULE32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getUint32(sp, true) <= b ? -1 : 0, true);
              break;
            case OP_UGE:
            case OP_UGE32:
              b = dv.getUint32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getUint32(sp, true) >= b ? -1 : 0, true);
              break;
            case OP_FEQ:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getFloat32(sp, true) === b ? -1 : 0, true);
              break;
            case OP_FNE:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getFloat32(sp, true) !== b ? -1 : 0, true);
              break;
            case OP_FLT:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getFloat32(sp, true) < b ? -1 : 0, true);
              break;
            case OP_FGT:
              b = dv.getFloat32(sp, true); sp += 4;
              dv.setInt32(sp, dv.getFloat32(sp, true) > b ? -1 : 0, true);
              break;
            case OP_I8FETCH:
              addr = dv.getUint32(sp, true);
              dv.setInt32(sp, dv.getInt8(addr), true);
              break;
            case OP_U8FETCH:
              addr = dv.getUint32(sp, true);
              dv.setInt32(sp, mem[addr], true);
              break;
            case OP_I8STORE:
              addr = dv.getUint32(sp, true);
              v = dv.getInt32(sp + 4, true);
              sp += 8;
              mem[addr] = v & 0xFF;
              break;
            case OP_FETCH:
            case OP_FETCH32:
              addr = dv.getUint32(sp, true);
              dv.setInt32(sp, dv.getInt32(addr, true), true);
              break;
            case OP_STORE:
            case OP_STORE32:
              addr = dv.getUint32(sp, true);
              v = dv.getInt32(sp + 4, true);
              sp += 8;
              dv.setInt32(addr, v, true);
              break;
            case OP_U16FETCH:
              addr = dv.getUint32(sp, true);
              dv.setInt32(sp, dv.getUint16(addr, true), true);
              break;
            case OP_I16FETCH:
              addr = dv.getUint32(sp, true);
              dv.setInt32(sp, dv.getInt16(addr, true), true);
              break;
            case OP_STORE16:
              addr = dv.getUint32(sp, true);
              v = dv.getInt32(sp + 4, true);
              sp += 8;
              dv.setUint16(addr, v & 0xFFFF, true);
              break;
            case OP_BCOPY: {
              var len = dv.getUint32(sp, true);
              var src = dv.getUint32(sp + 4, true);
              var dst = dv.getUint32(sp + 8, true);
              sp += 12;
              if (len > 0) mem.copyWithin(dst, src, src + len);
              break;
            }
            case OP_SEXT:
              dv.setInt32(sp, dv.getInt8(sp), true);
              break;
            case OP_W2F:
              dv.setFloat32(sp, dv.getInt32(sp, true), true);
              break;
            case OP_F2W:
              dv.setInt32(sp, dv.getFloat32(sp, true) | 0, true);
              break;
            default:
              throw new Error("Unsupported opcode: " + op);
          }
        }
        return "running";
      }
    };
    return vm;
  }

  var VGA_PAL = (function () {
    var pal = new Uint32Array(256);
    var c16 = [
      0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
      0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
    ];
    var i;
    for (i = 0; i < 16; i++) pal[i] = c16[i];
    for (i = 16; i < 232; i++) {
      var idx = i - 16;
      var r = Math.floor(idx / 36);
      var g = Math.floor((idx % 36) / 6);
      var b = idx % 6;
      pal[i] = ((r ? r * 40 + 55 : 0) << 16) |
               ((g ? g * 40 + 55 : 0) << 8) |
               (b ? b * 40 + 55 : 0);
    }
    for (i = 232; i < 256; i++) {
      var v = (i - 232) * 10 + 8;
      pal[i] = (v << 16) | (v << 8) | v;
    }
    return pal;
  })();

  function vgaNearest(r, g, b) {
    var best = 0, bestD = 1e9;
    for (var i = 0; i < 256; i++) {
      var c = VGA_PAL[i];
      var dr = r - ((c >> 16) & 255);
      var dg = g - ((c >> 8) & 255);
      var db = b - (c & 255);
      var d = dr * dr + dg * dg + db * db;
      if (d < bestD) { bestD = d; best = i; }
    }
    return best;
  }

  function decodeBase64(b64) {
    var bin = atob(b64.replace(/\s+/g, ""));
    var out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function createBrowserHost(el, opts) {
    var FONT_FAMILY = 'ui-monospace, "SFMono-Regular", Menlo, Consolas, "Liberation Mono", monospace';
    var TERM_CELL_W = 8;
    var TERM_CELL_H = 16;
    var initW = opts.width || 320;
    var initH = opts.height || 240;
    var scale = opts.scale || 2;
    var w = initW;
    var h = initH;
    var depth = 24;
    var color = 0xFFFFFF;
    var gfxActive = false;
    var startMs = Date.now();
    var ticks = 0;
    var nextVsyncTs = 0;
    var keyQueue = [];
    var keysDown = {};
    var resumeFn = null;
    var dispW = Math.max(160, Math.round(initW * scale));
    var dispH = Math.max(160, Math.round(initH * scale));
    var termCols = Math.max(20, Math.floor(dispW / TERM_CELL_W));
    var termRows = Math.max(10, Math.floor(dispH / TERM_CELL_H));
    var termPixelW = termCols * TERM_CELL_W;
    var termPixelH = termRows * TERM_CELL_H;
    var tx = 0, ty = 0, tfg = 7, tbg = 0, tscreenBg = 0, tcursor = 1;
    var termBuf = [];
    var termFg = [];
    var termBg = [];
    var termDirty = false;
    function makeRow(ch, fg, bg) {
      return {
        ch: new Array(termCols).fill(ch),
        fg: new Uint8Array(termCols).fill(fg),
        bg: new Uint8Array(termCols).fill(bg)
      };
    }
    for (var r = 0; r < termRows; r++) {
      var row = makeRow(" ", 7, 0);
      termBuf.push(row.ch); termFg.push(row.fg); termBg.push(row.bg);
    }

    var rootEl = el.shadowRoot || el.attachShadow({ mode: "open" });
    rootEl.innerHTML = "";
    var wrap = document.createElement("div");
    wrap.style.cssText = "position:relative;overflow:hidden;background:#000;outline:none;font-family:" + FONT_FAMILY + ";";
    wrap.tabIndex = 0;

    var gfxWrap = document.createElement("div");
    gfxWrap.style.cssText = "position:absolute;left:0;top:0;transform-origin:top left;display:none;";

    var bgCanvas = document.createElement("canvas");
    bgCanvas.width = w;
    bgCanvas.height = h;
    bgCanvas.style.cssText = "position:absolute;left:0;top:0;width:" + w + "px;height:" + h + "px;image-rendering:pixelated;";
    var ctx = bgCanvas.getContext("2d");
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);

    ctx.font = "13px " + FONT_FAMILY;
    var m13 = ctx.measureText("MMMMMMMMMM").width / 10;
    var termFontPx = (m13 > 0) ? +(13 * TERM_CELL_W / m13).toFixed(3) : 13;
    ctx.font = "8px " + FONT_FAMILY;
    var gfxAdv8 = ctx.measureText("M").width || 4.8;

    var termPre = document.createElement("pre");
    termPre.setAttribute("role", "region");
    termPre.setAttribute("aria-label", "Terminal");
    termPre.style.cssText = "position:absolute;left:0;top:0;margin:0;padding:0;width:" + termPixelW + "px;height:" + termPixelH + "px;" +
      "background:#000;color:#aaa;font:" + termFontPx + "px/" + TERM_CELL_H + "px " + FONT_FAMILY + ";" +
      "font-variant-ligatures:none;white-space:pre;overflow:hidden;user-select:text;";

    gfxWrap.appendChild(bgCanvas);

    var sprCanvases = [];
    var sprImgs = [];
    for (var si = 0; si < 64; si++) {
      var sc = document.createElement("canvas");
      sc.style.cssText = "position:absolute;left:0;top:0;display:none;pointer-events:none;image-rendering:pixelated;z-index:" + (64 - si) + ";";
      gfxWrap.appendChild(sc);
      sprCanvases.push(sc);
    }

    wrap.appendChild(gfxWrap);
    wrap.appendChild(termPre);

    function updateLayout() {
      el.style.display = "inline-block";
      if (gfxActive) {
        var gw = Math.round(w * scale);
        var gh = Math.round(h * scale);
        wrap.style.width = gw + "px";
        wrap.style.height = gh + "px";
        el.style.width = gw + "px";
        el.style.height = gh + "px";
        gfxWrap.style.width = w + "px";
        gfxWrap.style.height = h + "px";
        gfxWrap.style.transform = "scale(" + scale + ")";
        gfxWrap.style.display = "block";
        termPre.style.display = "none";
        termPre.setAttribute("aria-hidden", "true");
      } else {
        wrap.style.width = termPixelW + "px";
        wrap.style.height = termPixelH + "px";
        el.style.width = termPixelW + "px";
        el.style.height = termPixelH + "px";
        gfxWrap.style.display = "none";
        termPre.style.display = "block";
        termPre.removeAttribute("aria-hidden");
      }
    }
    updateLayout();
    rootEl.appendChild(wrap);

    function termCss(idx) {
      var rgb = VGA_PAL[idx & 15];
      return "#" + ("000000" + rgb.toString(16)).slice(-6);
    }

    function escHtml(s) {
      return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function flushTerm() {
      termDirty = false;
      termPre.style.background = termCss(tscreenBg);
      var html = "";
      var curX = (tcursor && !gfxActive && tx >= 0 && tx < termCols && ty >= 0 && ty < termRows) ? tx : -1;
      var curY = (curX >= 0) ? ty : -1;
      for (var y = 0; y < termRows; y++) {
        var ch = termBuf[y], fg = termFg[y], bg = termBg[y];
        var x = 0;
        while (x < termCols) {
          var cf = fg[x], cb = bg[x];
          var isCur = (y === curY && x === curX);
          var startX = x;
          var run = ch[x++];
          if (!isCur) {
            while (x < termCols && fg[x] === cf && bg[x] === cb && !(y === curY && x === curX)) {
              run += ch[x++];
            }
          }
          var rw = (x - startX) * TERM_CELL_W;
          var st = "display:inline-block;width:" + rw + "px;height:" + TERM_CELL_H + "px;line-height:" +
            TERM_CELL_H + "px;vertical-align:top;color:" + termCss(cf) + ";background:" + termCss(cb);
          if (isCur) {
            st += ";box-shadow:inset 0 -2px 0 " + termCss(cf);
          }
          html += "<span style=\"" + st + "\">" + escHtml(run) + "</span>";
        }
        if (y + 1 < termRows) html += "\n";
      }
      termPre.innerHTML = html;
    }
    flushTerm();

    function renderTerm() {
      if (!termDirty) {
        termDirty = true;
        setTimeout(flushTerm, 0);
      }
    }

    function scrollTerm() {
      termBuf.shift(); termFg.shift(); termBg.shift();
      var row = makeRow(" ", tfg, tbg);
      termBuf.push(row.ch); termFg.push(row.fg); termBg.push(row.bg);
      ty = termRows - 1;
    }

    function termPutChar(c) {
      if (c === 10) {
        tx = 0;
        ty++;
      } else if (c === 13) {
        tx = 0;
      } else if (c === 8) {
        if (tx > 0) tx--;
      } else {
        if (tx >= termCols) { tx = 0; ty++; }
        if (ty >= termRows) scrollTerm();
        termBuf[ty][tx] = String.fromCharCode(c);
        termFg[ty][tx] = tfg;
        termBg[ty][tx] = tbg;
        tx++;
      }
      if (ty >= termRows) scrollTerm();
      renderTerm();
    }

    function cssColor(c) {
      var rgb = (depth === 8) ? VGA_PAL[c & 255] : (c & 0xFFFFFF);
      return "#" + ("000000" + rgb.toString(16)).slice(-6);
    }

    window.addEventListener("keydown", function (e) {
      if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA")) return;
      keysDown[e.code] = true;
      var k = 0;
      if (e.key.length === 1) k = e.key.charCodeAt(0);
      else if (e.key === "Enter") k = 13;
      else if (e.key === "Backspace") k = 8;
      else if (e.key === "Escape") k = 27;
      else if (e.key === "ArrowUp") k = 128;
      else if (e.key === "ArrowDown") k = 129;
      else if (e.key === "ArrowLeft") k = 130;
      else if (e.key === "ArrowRight") k = 131;
      if (k !== 0) {
        keyQueue.push(k);
        e.preventDefault();
        if (resumeFn) { var fn = resumeFn; resumeFn = null; fn(); }
      }
    });
    window.addEventListener("keyup", function (e) { keysDown[e.code] = false; });

    var audioCtx = null;
    var voices = [null, null, null, null];

    return {
      setResume: function (fn) { resumeFn = fn; },
      onYield: function (vm, kind, arg) {
        if (kind === "vsync") {
          ticks++;
          requestAnimationFrame(function () { if (resumeFn) { var f = resumeFn; resumeFn = null; f(); } });
        } else if (kind === "sleep") {
          setTimeout(function () { if (resumeFn) { var f = resumeFn; resumeFn = null; f(); } }, arg);
        }
      },
      externs: {
        putchar: function (vm) { var c = vm.popI32() & 0xFF; termPutChar(c); vm.pushI32(c); },
        web_inkey: function (vm) { vm.pushI32(keyQueue.length > 0 ? keyQueue.shift() : 0); },
        web_key: function (vm) {
          if (keyQueue.length > 0) vm.pushI32(keyQueue.shift());
          else vm.retryExtern();
        },
        web_termwidth: function (vm) { vm.pushI32(termCols); },
        web_termheight: function (vm) { vm.pushI32(termRows); },
        web_termx: function (vm) { vm.pushI32(tx); },
        web_termy: function (vm) { vm.pushI32(ty); },
        web_termclear: function () {
          for (var y = 0; y < termRows; y++) {
            termBuf[y].fill(" "); termFg[y].fill(tfg); termBg[y].fill(tbg);
          }
          tscreenBg = tbg;
          tx = 0; ty = 0; renderTerm();
        },
        web_termcleareol: function () {
          for (var x = tx; x < termCols; x++) {
            termBuf[ty][x] = " "; termFg[ty][x] = tfg; termBg[ty][x] = tbg;
          }
          renderTerm();
        },
        web_termcleareos: function () {
          for (var x = tx; x < termCols; x++) {
            termBuf[ty][x] = " "; termFg[ty][x] = tfg; termBg[ty][x] = tbg;
          }
          for (var y = ty + 1; y < termRows; y++) {
            termBuf[y].fill(" "); termFg[y].fill(tfg); termBg[y].fill(tbg);
          }
          renderTerm();
        },
        web_termgoto: function (vm) {
          var y = vm.popI32(), x = vm.popI32();
          tx = Math.max(0, Math.min(termCols - 1, x));
          ty = Math.max(0, Math.min(termRows - 1, y));
          if (tcursor) renderTerm();
        },
        web_termcursor: function (vm) {
          var c = vm.popI32() ? 1 : 0;
          if (tcursor !== c) { tcursor = c; renderTerm(); }
        },
        web_termfg: function (vm) { tfg = vm.popI32() & 15; },
        web_termbg: function (vm) { tbg = vm.popI32() & 15; },
        web_termreset: function () { tfg = 7; tbg = 0; renderTerm(); },
        web_gfxwidth: function (vm) { vm.pushI32(w); },
        web_gfxheight: function (vm) { vm.pushI32(h); },
        web_gfxdepth: function (vm) { vm.pushI32(depth); },
        web_gfxcharw: function (vm) { vm.pushI32(Math.max(1, Math.round(gfxAdv8))); },
        web_gfxvsync: function (vm) {
          vm.yieldNow();
          function waitFrame(ts) {
            if (nextVsyncTs === 0) nextVsyncTs = ts;
            if (ts + 1.5 < nextVsyncTs) {
              requestAnimationFrame(waitFrame);
              return;
            }
            nextVsyncTs = Math.max(ts, nextVsyncTs + 1000 / 60);
            ticks++;
            if (resumeFn) { var f = resumeFn; resumeFn = null; f(); }
          }
          requestAnimationFrame(waitFrame);
        },
        web_gfxticks: function (vm) { vm.pushI32(ticks | 0); },
        web_gfxmode: function (vm) {
          var m = vm.popI32();
          depth = (m === 1) ? 8 : 24;
          gfxActive = true;
          updateLayout();
          ctx.fillStyle = "#000";
          ctx.fillRect(0, 0, w, h);
          ctx.fillStyle = cssColor(color);
          ctx.strokeStyle = cssColor(color);
        },
        web_gfxdone: function () {
          gfxActive = false;
          w = initW; h = initH;
          bgCanvas.width = w; bgCanvas.height = h;
          bgCanvas.style.width = w + "px"; bgCanvas.style.height = h + "px";
          for (var si = 0; si < 64; si++) sprCanvases[si].style.display = "none";
          updateLayout();
          flushTerm();
        },
        web_gfxinit: function (vm) {
          var d = vm.popI32(), gh = vm.popI32(), gw = vm.popI32();
          depth = (d <= 8) ? 8 : 24;
          if (gw > 0 && gh > 0) {
            w = gw; h = gh;
          } else {
            w = initW; h = initH;
          }
          bgCanvas.width = w; bgCanvas.height = h;
          bgCanvas.style.width = w + "px"; bgCanvas.style.height = h + "px";
          gfxActive = true;
          updateLayout();
          ctx.fillStyle = "#000";
          ctx.fillRect(0, 0, w, h);
          ctx.fillStyle = cssColor(color);
          ctx.strokeStyle = cssColor(color);
        },
        web_gfxrgb: function (vm) {
          var b = vm.popI32() & 255, g = vm.popI32() & 255, r = vm.popI32() & 255;
          vm.pushI32(depth === 8 ? vgaNearest(r, g, b) : ((r << 16) | (g << 8) | b));
        },
        web_gfxcolor: function (vm) { color = vm.popI32(); ctx.fillStyle = cssColor(color); ctx.strokeStyle = cssColor(color); },
        web_gfxpx: function (vm) { var y = vm.popI32(), x = vm.popI32(); ctx.fillRect(x, y, 1, 1); },
        web_gfxgetpx: function (vm) {
          var y = vm.popI32(), x = vm.popI32();
          if (x < 0 || y < 0 || x >= w || y >= h) { vm.pushI32(-1); return; }
          var p = ctx.getImageData(x, y, 1, 1).data;
          vm.pushI32(depth === 8 ? vgaNearest(p[0], p[1], p[2]) : ((p[0] << 16) | (p[1] << 8) | p[2]));
        },
        web_gfxhline: function (vm) { var len = vm.popI32(), y = vm.popI32(), x = vm.popI32(); ctx.fillRect(x, y, len, 1); },
        web_gfxvline: function (vm) { var len = vm.popI32(), y = vm.popI32(), x = vm.popI32(); ctx.fillRect(x, y, 1, len); },
        web_gfxline: function (vm) {
          var y2 = vm.popI32(), x2 = vm.popI32(), y1 = vm.popI32(), x1 = vm.popI32();
          ctx.beginPath(); ctx.moveTo(x1 + 0.5, y1 + 0.5); ctx.lineTo(x2 + 0.5, y2 + 0.5); ctx.stroke();
        },
        web_gfxrect: function (vm) {
          var rh = vm.popI32(), rw = vm.popI32(), y = vm.popI32(), x = vm.popI32();
          ctx.strokeRect(x + 0.5, y + 0.5, rw - 1, rh - 1);
        },
        web_gfxfillrect: function (vm) {
          var rh = vm.popI32(), rw = vm.popI32(), y = vm.popI32(), x = vm.popI32();
          ctx.fillRect(x, y, rw, rh);
        },
        web_gfxclear: function () { ctx.fillStyle = cssColor(color); ctx.fillRect(0, 0, w, h); },
        web_gfxcircle: function (vm) {
          var r = vm.popI32(), cy = vm.popI32(), cx = vm.popI32();
          ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.stroke();
        },
        web_gfxfill: function (vm) {
          var sy = vm.popI32(), sx = vm.popI32();
          if (sx < 0 || sy < 0 || sx >= w || sy >= h) return;
          var idata = ctx.getImageData(0, 0, w, h);
          var d = idata.data;
          var rgb = (depth === 8) ? VGA_PAL[color & 255] : (color & 0xFFFFFF);
          var fr = (rgb >> 16) & 255, fg = (rgb >> 8) & 255, fb = rgb & 255;
          var o0 = (sy * w + sx) * 4;
          var sr = d[o0], sg = d[o0 + 1], sb = d[o0 + 2];
          if (sr === fr && sg === fg && sb === fb) return;
          var q = [sx, sy];
          while (q.length > 0) {
            var y = q.pop(), x = q.pop();
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            var o = (y * w + x) * 4;
            if (d[o] !== sr || d[o + 1] !== sg || d[o + 2] !== sb) continue;
            d[o] = fr; d[o + 1] = fg; d[o + 2] = fb; d[o + 3] = 255;
            q.push(x + 1, y, x - 1, y, x, y + 1, x, y - 1);
          }
          ctx.putImageData(idata, 0, 0);
        },
        web_gfxtext: function (vm) {
          var len = vm.popI32(), sAddr = vm.popU32(), y = vm.popI32(), x = vm.popI32();
          var txt = "";
          for (var i = 0; i < len; i++) txt += String.fromCharCode(vm.mem[sAddr + i]);
          ctx.font = "8px " + FONT_FAMILY;
          ctx.textBaseline = "alphabetic";
          ctx.fillStyle = cssColor(color);
          ctx.fillText(txt, x, y + 6.5);
        },
        web_sprimg: function (vm) {
          var ncolors = vm.popI32(), palAddr = vm.popU32(), dataAddr = vm.popU32();
          var sh = vm.popI32(), sw = vm.popI32(), imgId = vm.popI32();
          if (imgId < 0 || imgId >= 256 || sw <= 0 || sw > 64 || sh <= 0 || sh > 64 || ncolors <= 0) {
            vm.pushI32(0); return;
          }
          var palStride = (depth <= 8) ? 1 : 3;
          var palRgb = new Uint32Array(ncolors + 1);
          for (var i = 0; i < ncolors; i++) {
            if (palStride === 1) {
              palRgb[i + 1] = VGA_PAL[vm.mem[palAddr + i]];
            } else {
              var o = palAddr + i * 3;
              palRgb[i + 1] = (vm.mem[o] << 16) | (vm.mem[o + 1] << 8) | vm.mem[o + 2];
            }
          }
          var c = document.createElement("canvas");
          c.width = sw; c.height = sh;
          var sctx = c.getContext("2d");
          var idata = sctx.createImageData(sw, sh);
          for (var p = 0; p < sw * sh; p++) {
            var idx = vm.mem[dataAddr + p];
            var off = p * 4;
            if (idx === 0 || idx > ncolors) {
              idata.data[off + 3] = 0;
            } else {
              var rgb = palRgb[idx];
              idata.data[off] = (rgb >> 16) & 255;
              idata.data[off + 1] = (rgb >> 8) & 255;
              idata.data[off + 2] = rgb & 255;
              idata.data[off + 3] = 255;
            }
          }
          sctx.putImageData(idata, 0, 0);
          sprImgs[imgId] = c;
          vm.pushI32(-1);
        },
        web_sprset: function (vm) {
          var imgId = vm.popI32(), id = vm.popI32();
          if (id < 0 || id >= 64 || !sprImgs[imgId]) return;
          var src = sprImgs[imgId], dst = sprCanvases[id];
          dst.width = src.width; dst.height = src.height;
          dst.style.width = src.width + "px"; dst.style.height = src.height + "px";
          dst.getContext("2d").drawImage(src, 0, 0);
        },
        web_sprpos: function (vm) {
          var y = vm.popI32(), x = vm.popI32(), id = vm.popI32();
          if (id < 0 || id >= 64) return;
          sprCanvases[id].style.transform = "translate(" + x + "px," + y + "px)";
        },
        web_sprshow: function (vm) {
          var on = vm.popI32(), id = vm.popI32();
          if (id < 0 || id >= 64) return;
          sprCanvases[id].style.display = on ? "block" : "none";
        },
        web_sfxplay: function (vm) {
          var dur = vm.popI32(), wave = vm.popI32(), vol = vm.popI32(), freq = vm.popI32(), ch = vm.popI32();
          if (ch < 0 || ch >= 4 || typeof AudioContext === "undefined") return;
          if (!audioCtx) audioCtx = new AudioContext();
          if (voices[ch]) { try { voices[ch].stop(); } catch (e) {} }
          var osc = audioCtx.createOscillator(), gain = audioCtx.createGain();
          var types = ["square", "sawtooth", "triangle", "sine"];
          osc.type = types[wave & 3];
          osc.frequency.value = Math.max(20, freq);
          gain.gain.value = Math.max(0, Math.min(1, vol / 255)) * 0.2;
          osc.connect(gain); gain.connect(audioCtx.destination);
          osc.start();
          if (dur > 0) osc.stop(audioCtx.currentTime + dur / 1000);
          voices[ch] = osc;
        },
        web_sfxstop: function (vm) {
          var ch = vm.popI32();
          if (ch >= 0 && ch < 4 && voices[ch]) { try { voices[ch].stop(); } catch (e) {} voices[ch] = null; }
        },
        web_joycount: function (vm) {
          var gps = (typeof navigator !== "undefined" && navigator.getGamepads) ? navigator.getGamepads() : [];
          var n = 0;
          for (var i = 0; i < gps.length; i++) if (gps[i]) n++;
          vm.pushI32(n);
        },
        web_joystate: function (vm) {
          var port = vm.popI32(), mask = 0;
          if (port === -1 || port === 0) {
            if (keysDown.ArrowUp || keysDown.KeyW) mask |= 1;
            if (keysDown.ArrowDown || keysDown.KeyS) mask |= 2;
            if (keysDown.ArrowLeft || keysDown.KeyA) mask |= 4;
            if (keysDown.ArrowRight || keysDown.KeyD) mask |= 8;
            if (keysDown.Space || keysDown.KeyZ) mask |= 16;
            if (keysDown.KeyX || keysDown.ShiftLeft) mask |= 32;
            if (keysDown.Tab) mask |= 64;
            if (keysDown.Enter) mask |= 128;
          }
          if (port >= 0 && typeof navigator !== "undefined" && navigator.getGamepads) {
            var gp = navigator.getGamepads()[port];
            if (gp) {
              if (gp.axes[1] < -0.5 || (gp.buttons[12] && gp.buttons[12].pressed)) mask |= 1;
              if (gp.axes[1] > 0.5 || (gp.buttons[13] && gp.buttons[13].pressed)) mask |= 2;
              if (gp.axes[0] < -0.5 || (gp.buttons[14] && gp.buttons[14].pressed)) mask |= 4;
              if (gp.axes[0] > 0.5 || (gp.buttons[15] && gp.buttons[15].pressed)) mask |= 8;
              if (gp.buttons[0] && gp.buttons[0].pressed) mask |= 16;
              if (gp.buttons[1] && gp.buttons[1].pressed) mask |= 32;
              if (gp.buttons[8] && gp.buttons[8].pressed) mask |= 64;
              if (gp.buttons[9] && gp.buttons[9].pressed) mask |= 128;
            }
          }
          vm.pushI32(mask);
        },
        web_sysmillis: function (vm) { vm.pushI32((Date.now() - startMs) | 0); },
        web_syssleep: function (vm) {
          var ms = vm.popI32();
          vm.yieldNow();
          setTimeout(function () { if (resumeFn) { var f = resumeFn; resumeFn = null; f(); } }, Math.max(0, ms));
        },
        web_sysexit: function (vm) { vm.halt(vm.popI32()); }
      }
    };
  }

  if (typeof HTMLElement !== "undefined" && typeof customElements !== "undefined" &&
      !customElements.get("ir4-screen")) {
    var Ir4Screen = function () { return Reflect.construct(HTMLElement, [], Ir4Screen); };
    Ir4Screen.prototype = Object.create(HTMLElement.prototype);
    Ir4Screen.prototype.connectedCallback = function () {
      var b64 = this.getAttribute("data-bin");
      if (!b64) return;
      var w = parseInt(this.getAttribute("width") || "320", 10);
      var h = parseInt(this.getAttribute("height") || "240", 10);
      var s = parseFloat(this.getAttribute("scale") || "2");
      var mem = parseInt(this.getAttribute("mem") || "1048576", 10);
      var host = createBrowserHost(this, { width: w, height: h, scale: s });
      var vm = createIr4VM(decodeBase64(b64), host, mem);
      function pump() {
        var st = vm.step(50000);
        if (st === "running") {
          setTimeout(pump, 0);
        } else if (st === "yield") {
          host.setResume(pump);
        }
      }
      host.setResume(pump);
      pump();
    };
    customElements.define("ir4-screen", Ir4Screen);
  }

  function runCli(bytes) {
    var out = "";
    var gfxW = 320, gfxH = 240, gfxD = 24, gfxCol = 0, ticks = 0;
    var gfxFb = new Int32Array(gfxW * gfxH);
    var tx = 0, ty = 0;
    function emitChar(c) {
      if (c === 10) { tx = 0; ty++; }
      else if (c === 13) { tx = 0; }
      else if (c === 8) { if (tx > 0) tx--; }
      else if (c >= 32) { tx++; }
      if (typeof process !== "undefined" && process.stdout) {
        process.stdout.write(String.fromCharCode(c));
      } else if (c === 10) {
        print(out);
        out = "";
      } else if (c !== 13) {
        out += String.fromCharCode(c);
      }
    }
    var host = {
      externs: {
        putchar: function (vm) { var c = vm.popI32() & 0xFF; emitChar(c); vm.pushI32(c); },
        web_inkey: function (vm) { vm.pushI32(0); },
        web_key: function (vm) { vm.pushI32(0); },
        web_termwidth: function (vm) { vm.pushI32(80); },
        web_termheight: function (vm) { vm.pushI32(30); },
        web_termx: function (vm) { vm.pushI32(tx); },
        web_termy: function (vm) { vm.pushI32(ty); },
        web_termclear: function () { tx = 0; ty = 0; },
        web_termcleareol: function () {},
        web_termcleareos: function () {},
        web_termgoto: function (vm) { ty = vm.popI32(); tx = vm.popI32(); },
        web_termcursor: function (vm) { vm.popI32(); },
        web_termfg: function (vm) { vm.popI32(); },
        web_termbg: function (vm) { vm.popI32(); },
        web_termreset: function () {},
        web_gfxwidth: function (vm) { vm.pushI32(gfxW); },
        web_gfxheight: function (vm) { vm.pushI32(gfxH); },
        web_gfxdepth: function (vm) { vm.pushI32(gfxD); },
        web_gfxcharw: function (vm) { vm.pushI32(5); },
        web_gfxvsync: function () { ticks++; },
        web_gfxticks: function (vm) { vm.pushI32(ticks++); },
        web_gfxmode: function (vm) { gfxD = (vm.popI32() === 1) ? 8 : 24; },
        web_gfxdone: function () {},
        web_gfxinit: function (vm) {
          gfxD = vm.popI32(); gfxH = vm.popI32(); gfxW = vm.popI32();
          if (gfxW > 0 && gfxH > 0) gfxFb = new Int32Array(gfxW * gfxH);
        },
        web_gfxrgb: function (vm) {
          var b = vm.popI32() & 255, g = vm.popI32() & 255, r = vm.popI32() & 255;
          vm.pushI32(gfxD === 8 ? vgaNearest(r, g, b) : ((r << 16) | (g << 8) | b));
        },
        web_gfxcolor: function (vm) { gfxCol = vm.popI32(); },
        web_gfxpx: function (vm) {
          var y = vm.popI32(), x = vm.popI32();
          if (x >= 0 && y >= 0 && x < gfxW && y < gfxH) gfxFb[y * gfxW + x] = gfxCol;
        },
        web_gfxgetpx: function (vm) {
          var y = vm.popI32(), x = vm.popI32();
          vm.pushI32((x < 0 || y < 0 || x >= gfxW || y >= gfxH) ? -1 : gfxFb[y * gfxW + x]);
        },
        web_gfxhline: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); },
        web_gfxvline: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); },
        web_gfxline: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); vm.popI32(); },
        web_gfxrect: function (vm) {
          var rh = vm.popI32(), rw = vm.popI32(), y = vm.popI32(), x = vm.popI32();
          for (var xx = x; xx < x + rw; xx++) {
            if (xx >= 0 && xx < gfxW) {
              if (y >= 0 && y < gfxH) gfxFb[y * gfxW + xx] = gfxCol;
              if (y + rh - 1 >= 0 && y + rh - 1 < gfxH) gfxFb[(y + rh - 1) * gfxW + xx] = gfxCol;
            }
          }
          for (var yy = y; yy < y + rh; yy++) {
            if (yy >= 0 && yy < gfxH) {
              if (x >= 0 && x < gfxW) gfxFb[yy * gfxW + x] = gfxCol;
              if (x + rw - 1 >= 0 && x + rw - 1 < gfxW) gfxFb[yy * gfxW + x + rw - 1] = gfxCol;
            }
          }
        },
        web_gfxfillrect: function (vm) {
          var rh = vm.popI32(), rw = vm.popI32(), y = vm.popI32(), x = vm.popI32();
          for (var yy = y; yy < y + rh; yy++)
            for (var xx = x; xx < x + rw; xx++)
              if (xx >= 0 && yy >= 0 && xx < gfxW && yy < gfxH) gfxFb[yy * gfxW + xx] = gfxCol;
        },
        web_gfxclear: function () { gfxFb.fill(gfxCol); },
        web_gfxcircle: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); },
        web_gfxfill: function (vm) {
          var sy = vm.popI32(), sx = vm.popI32();
          if (sx < 0 || sy < 0 || sx >= gfxW || sy >= gfxH) return;
          var target = gfxFb[sy * gfxW + sx];
          if (target === gfxCol) return;
          var q = [sx, sy];
          while (q.length > 0) {
            var y = q.pop(), x = q.pop();
            if (x < 0 || y < 0 || x >= gfxW || y >= gfxH) continue;
            if (gfxFb[y * gfxW + x] !== target) continue;
            gfxFb[y * gfxW + x] = gfxCol;
            q.push(x + 1, y, x - 1, y, x, y + 1, x, y - 1);
          }
        },
        web_gfxtext: function (vm) { vm.popI32(); vm.popU32(); vm.popI32(); vm.popI32(); },
        web_sprimg: function (vm) {
          var n = vm.popI32(); vm.popU32(); vm.popU32();
          var sh = vm.popI32(), sw = vm.popI32(), id = vm.popI32();
          vm.pushI32((id >= 0 && id < 256 && sw > 0 && sw <= 64 && sh > 0 && sh <= 64 && n > 0) ? -1 : 0);
        },
        web_sprset: function (vm) { vm.popI32(); vm.popI32(); },
        web_sprpos: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); },
        web_sprshow: function (vm) { vm.popI32(); vm.popI32(); },
        web_sfxplay: function (vm) { vm.popI32(); vm.popI32(); vm.popI32(); vm.popI32(); vm.popI32(); },
        web_sfxstop: function (vm) { vm.popI32(); },
        web_joycount: function (vm) { vm.pushI32(0); },
        web_joystate: function (vm) { vm.popI32(); vm.pushI32(0); },
        web_sysmillis: function (vm) { vm.pushI32(Date.now() & 0x7FFFFFFF); },
        web_syssleep: function (vm) { vm.popI32(); },
        web_sysexit: function (vm) { vm.halt(vm.popI32()); }
      }
    };
    var vm = createIr4VM(bytes, host);
    while (vm.step(1000000) !== "halted") {}
    if (out.length > 0 && typeof print === "function") print(out);
    if (typeof process !== "undefined" && process.stdout) {
      process.stdout.write(String(vm.exitCode) + "\n");
      process.exit(vm.exitCode);
    } else if (typeof print === "function") {
      print(String(vm.exitCode));
      if (vm.exitCode !== 0 && typeof quit === "function") quit(vm.exitCode);
    }
  }

  root.IR4VM = { parseIr4Bin: parseIr4Bin, createIr4VM: createIr4VM };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = root.IR4VM;
  }

  if (typeof process !== "undefined" && process.versions && process.versions.node &&
      typeof require !== "undefined" && require.main === module && process.argv[2]) {
    var fs = require("fs");
    runCli(new Uint8Array(fs.readFileSync(process.argv[2])));
  } else if (typeof readFile === "function" && cliArgs && cliArgs.length > 0) {
    var bin = readFile(cliArgs[0], "binary");
    runCli(bin instanceof Uint8Array ? bin : new Uint8Array(bin));
  }
})(typeof globalThis !== "undefined" ? globalThis : this,
   typeof arguments !== "undefined" ? arguments : null);

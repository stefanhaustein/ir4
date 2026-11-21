{ ir4p.pas - Unoptimized single-pass RV32 (virt ELF32) reference compiler for IR4(th).
  Written in classic Pascal (no SysUtils/Classes, no dynamic arrays or AnsiStrings). }
program ir4p;

const
  VIRT_BASE  = $80000000;
  VIRT_UART0 = $10000000;
  VIRT_TEST  = $00100000;

  RV_ZERO = 0;
  RV_RA   = 1;
  RV_SP   = 2;
  RV_T0   = 5;
  RV_T1   = 6;
  RV_T2   = 7;
  RV_FP   = 8;
  RV_DSP  = 9;   { s1: Forth data stack pointer (4-byte slots, grows down) }
  RV_A0   = 10;
  RV_DB   = 18;  { s2: Data segment base pointer }
  RV_FT0  = 0;
  RV_FT1  = 1;

  MAX_SRC       = 16;
  MAX_SEEN      = 128;
  MAX_ROOTS     = 16;
  MAX_CODE      = 131072;
  MAX_DATA      = 65536;
  MAX_FUNCS     = 512;
  MAX_PARAMS    = 16;
  MAX_OUTS      = 8;
  MAX_LOCALS    = 64;
  MAX_BUFFERS   = 256;
  MAX_CONSTS    = 512;
  MAX_INITS     = 64;
  MAX_TSTACK    = 64;
  MAX_BLOCKS    = 32;
  MAX_LABELS    = 128;
  MAX_LREFS     = 256;
  MAX_JTAB      = 128;
  MAX_FWDCALLS  = 1024;
  MAX_HRELOCS   = 256;
  MAX_DRELOCS   = 256;

type
  TShortName = string[63];
  TPathStr   = string[255];

  TStackType = (tyNone, tyW, tyA, tyF, tyI32);
  TTypeArray = array[0..MAX_TSTACK - 1] of TStackType;

  TFloatBits = record
    case Boolean of
      False: (f: Single);
      True:  (i: LongInt);
  end;

  TSrcFile = record
    fp: file;
    buf: array[0..511] of Byte;
    bufPos: Integer;
    bufLen: Integer;
    pushback: Integer;
    path: TPathStr;
    lineNum: Integer;
    reqGroup: Integer;
    reqRank: Integer;
  end;

  TFuncSig = record
    name: TShortName;
    inTypes: array[0..MAX_PARAMS - 1] of TStackType;
    inNames: array[0..MAX_PARAMS - 1] of TShortName;
    inCount: Integer;
    outTypes: array[0..MAX_OUTS - 1] of TStackType;
    outCount: Integer;
    addr: Cardinal;
    hasAddr: Boolean;
    isDefined: Boolean;
    isDefault: Boolean;
    isExtern: Boolean;
    reqGroup: Integer;
    reqRank: Integer;
  end;

  TLocalVar = record
    name: TShortName;
    ty: TStackType;
    fpOff: LongInt;
    size: Cardinal;
  end;

  TBufferSym = record
    name: TShortName;
    offset: Cardinal;
    ty: TStackType;
  end;

  TConstSym = record
    name: TShortName;
    ty: TStackType;
    wVal: LongInt;
    fVal: Single;
    isDefault: Boolean;
    reqGroup: Integer;
    reqRank: Integer;
  end;

  TBlockKind = (bkIf, bkBegin);
  TBlockInfo = record
    kind: TBlockKind;
    hasElse: Boolean;
    savedUnreach: Boolean;
    ifUnreach: Boolean;
    patchElse: Cardinal;
    patchEnd: Cardinal;
    loopAddr: Cardinal;
    savedStack: TTypeArray;
    savedLen: Integer;
    elseStack: TTypeArray;
    elseLen: Integer;
  end;

  TLabelInfo = record
    name: TShortName;
    defined: Boolean;
    addr: Cardinal;
  end;

  TLabelRef = record
    patchOff: Cardinal;
    labelIdx: Integer;
  end;

  TFwdCall = record
    patchOff: Cardinal;
    funcIdx: Integer;
    isFnPtr: Boolean;
  end;

  TDataReloc = record
    dataOff: Cardinal;
    targetVal: Cardinal;
    isCode: Boolean;
  end;

var
  srcStack: array[0..MAX_SRC - 1] of TSrcFile;
  srcDepth: Integer;
  seenFiles: array[0..MAX_SEEN - 1] of TPathStr;
  seenCount: Integer;
  extraLibRoots: array[0..MAX_ROOTS - 1] of TPathStr;
  extraRootCount: Integer;
  nextReqGroup: Integer;

  codeBuf: array[0..MAX_CODE - 1] of Byte;
  codeLen: Cardinal;
  dataBuf: array[0..MAX_DATA - 1] of Byte;
  dataLen: Cardinal;
  dataInitSize: Cardinal;

  funcs: array[0..MAX_FUNCS - 1] of TFuncSig;
  funcCount: Integer;
  buffers: array[0..MAX_BUFFERS - 1] of TBufferSym;
  bufferCount: Integer;
  consts: array[0..MAX_CONSTS - 1] of TConstSym;
  constCount: Integer;
  initFuncs: array[0..MAX_INITS - 1] of Integer;
  initCount: Integer;

  locals: array[0..MAX_LOCALS - 1] of TLocalVar;
  localCount: Integer;
  tstack: TTypeArray;
  tstackLen: Integer;
  curOuts: array[0..MAX_OUTS - 1] of TStackType;
  curOutCount: Integer;
  blocks: array[0..MAX_BLOCKS - 1] of TBlockInfo;
  blockCount: Integer;
  labels: array[0..MAX_LABELS - 1] of TLabelInfo;
  labelCount: Integer;
  labelRefs: array[0..MAX_LREFS - 1] of TLabelRef;
  labelRefCount: Integer;

  fwdCalls: array[0..MAX_FWDCALLS - 1] of TFwdCall;
  fwdCallCount: Integer;
  heapRelocs: array[0..MAX_HRELOCS - 1] of Cardinal;
  heapRelocCount: Integer;
  dataRelocs: array[0..MAX_DRELOCS - 1] of TDataReloc;
  dataRelocCount: Integer;

{ --- Error & String Helpers --- }

procedure Die(const msg: string);
begin
  if srcDepth > 0 then
    Writeln(StdErr, 'ir4p error (', srcStack[srcDepth - 1].path, ':',
            srcStack[srcDepth - 1].lineNum, '): ', msg)
  else
    Writeln(StdErr, 'ir4p error: ', msg);
  Halt(1);
end;

function IntToStrSimple(n: Integer): TShortName;
var
  s: TShortName;
begin
  Str(n, s);
  IntToStrSimple := s;
end;

{ Reinterpret IEEE-754 Single bits as a 32-bit signed integer. }
function FloatToBits(f: Single): LongInt;
var
  fb: TFloatBits;
begin
  fb.f := f;
  FloatToBits := fb.i;
end;

function TypeName(t: TStackType): TShortName;
begin
  case t of
    tyW:   TypeName := ':i';
    tyA:   TypeName := ':a';
    tyF:   TypeName := ':f';
    tyI32: TypeName := ':i32';
  else
    TypeName := '?';
  end;
end;

function ParseTypeTok(const tok: TShortName): TStackType;
begin
  if tok = ':i' then ParseTypeTok := tyW
  else if tok = ':a' then ParseTypeTok := tyA
  else if tok = ':f' then ParseTypeTok := tyF
  else if tok = ':i32' then ParseTypeTok := tyI32
  else ParseTypeTok := tyNone;
end;

{ --- Code & Data Buffer --- }

function CodePC: Cardinal;
begin
  CodePC := codeLen;
end;

procedure Emit8(b: Byte);
begin
  if codeLen >= MAX_CODE then Die('code buffer overflow');
  codeBuf[codeLen] := b;
  Inc(codeLen);
end;

procedure Emit32(w: Cardinal);
begin
  Emit8(Byte(w and $FF));
  Emit8(Byte((w shr 8) and $FF));
  Emit8(Byte((w shr 16) and $FF));
  Emit8(Byte((w shr 24) and $FF));
end;

procedure Patch32(off: Cardinal; w: Cardinal);
begin
  codeBuf[off]     := Byte(w and $FF);
  codeBuf[off + 1] := Byte((w shr 8) and $FF);
  codeBuf[off + 2] := Byte((w shr 16) and $FF);
  codeBuf[off + 3] := Byte((w shr 24) and $FF);
end;

procedure PutLE16(var buf: array of Byte; off: Integer; v: Word);
begin
  buf[off]     := Byte(v and $FF);
  buf[off + 1] := Byte((v shr 8) and $FF);
end;

procedure PutLE32(var buf: array of Byte; off: Integer; v: Cardinal);
begin
  buf[off]     := Byte(v and $FF);
  buf[off + 1] := Byte((v shr 8) and $FF);
  buf[off + 2] := Byte((v shr 16) and $FF);
  buf[off + 3] := Byte((v shr 24) and $FF);
end;

{ Zero-initialized allocation in dataBuf; updates dataLen (BSS) without advancing dataInitSize. }
function AllocData(sz: Cardinal; align: Cardinal): Cardinal;
var
  aligned, newLen, i: Cardinal;
begin
  aligned := dataLen;
  if align > 1 then begin
    if (aligned mod align) <> 0 then
      aligned := aligned + (align - (aligned mod align));
  end;
  newLen := aligned + sz;
  if newLen > MAX_DATA then Die('data buffer overflow');
  if newLen > dataLen then begin
    i := dataLen;
    while i < newLen do begin
      dataBuf[i] := 0;
      Inc(i);
    end;
    dataLen := newLen;
  end;
  AllocData := aligned;
end;

procedure EmitDataU8(b: Byte);
begin
  if dataLen >= MAX_DATA then Die('data buffer overflow');
  dataBuf[dataLen] := b;
  Inc(dataLen);
  dataInitSize := dataLen;
end;

procedure EmitDataU16(w: Word);
begin
  EmitDataU8(Byte(w and $FF));
  EmitDataU8(Byte((w shr 8) and $FF));
end;

procedure EmitDataU32(w: Cardinal);
begin
  EmitDataU8(Byte(w and $FF));
  EmitDataU8(Byte((w shr 8) and $FF));
  EmitDataU8(Byte((w shr 16) and $FF));
  EmitDataU8(Byte((w shr 24) and $FF));
end;

{ --- RV32 Instruction Encoders --- }

function EncR(f7, rs2, rs1, f3, rd, op: Cardinal): Cardinal;
begin
  EncR := (f7 shl 25) or (rs2 shl 20) or (rs1 shl 15) or (f3 shl 12) or (rd shl 7) or op;
end;

function EncI(imm: LongInt; rs1, f3, rd, op: Cardinal): Cardinal;
begin
  EncI := ((Cardinal(imm) and $FFF) shl 20) or (rs1 shl 15) or (f3 shl 12) or (rd shl 7) or op;
end;

function EncS(imm: LongInt; rs2, rs1, f3: Cardinal): Cardinal;
var
  u: Cardinal;
begin
  u := Cardinal(imm);
  EncS := (((u shr 5) and $7F) shl 25) or (rs2 shl 20) or (rs1 shl 15) or
          (f3 shl 12) or ((u and $1F) shl 7) or $23;
end;

function EncB(imm: LongInt; rs2, rs1, f3: Cardinal): Cardinal;
var
  u: Cardinal;
begin
  u := Cardinal(imm);
  EncB := (((u shr 12) and 1) shl 31) or (((u shr 5) and $3F) shl 25) or
          (rs2 shl 20) or (rs1 shl 15) or (f3 shl 12) or
          (((u shr 1) and $F) shl 8) or (((u shr 11) and 1) shl 7) or $63;
end;

function EncU(imm20, rd, op: Cardinal): Cardinal;
begin
  EncU := (imm20 and $FFFFF000) or (rd shl 7) or op;
end;

function EncJ(imm: LongInt; rd: Cardinal): Cardinal;
var
  u: Cardinal;
begin
  u := Cardinal(imm);
  EncJ := (((u shr 20) and 1) shl 31) or (((u shr 1) and $3FF) shl 21) or
          (((u shr 11) and 1) shl 20) or (((u shr 12) and $FF) shl 12) or
          (rd shl 7) or $6F;
end;

procedure RvAddi(rd, rs1: Cardinal; imm: LongInt);
begin
  Emit32(EncI(imm, rs1, 0, rd, $13));
end;

procedure RvLw(rd: Cardinal; off: LongInt; rs1: Cardinal);
begin
  Emit32(EncI(off, rs1, 2, rd, $03));
end;

procedure RvSw(rs2: Cardinal; off: LongInt; rs1: Cardinal);
begin
  Emit32(EncS(off, rs2, rs1, 2));
end;

{ Load a 32-bit constant into rd using ADDI when 12-bit signed, else LUI+ADDI. }
procedure RvLi(rd: Cardinal; val: Cardinal);
var
  sval: LongInt;
  hi: Cardinal;
  lo: LongInt;
begin
  sval := LongInt(val);
  if (sval >= -2048) and (sval <= 2047) then
    RvAddi(rd, RV_ZERO, sval)
  else begin
    hi := (val + $800) and $FFFFF000;
    lo := LongInt(val - hi);
    Emit32(EncU(hi, rd, $37));
    if lo <> 0 then
      RvAddi(rd, rd, lo);
  end;
end;

{ Push/pop 32-bit integer or float values on the dedicated data stack (s1). }
procedure PushReg(r: Cardinal);
begin
  RvAddi(RV_DSP, RV_DSP, -4);
  RvSw(r, 0, RV_DSP);
end;

procedure PopReg(r: Cardinal);
begin
  RvLw(r, 0, RV_DSP);
  RvAddi(RV_DSP, RV_DSP, 4);
end;

procedure PushF(fs: Cardinal);
begin
  RvAddi(RV_DSP, RV_DSP, -4);
  Emit32((EncS(0, fs, RV_DSP, 2) and not Cardinal($7F)) or $27); { FSW fs, 0(s1) }
end;

procedure PopF(fd: Cardinal);
begin
  Emit32(EncI(0, RV_DSP, 2, fd, $07)); { FLW fd, 0(s1) }
  RvAddi(RV_DSP, RV_DSP, 4);
end;

{ --- Lexer & Require Loader --- }

function OpenSrcFile(const path: TPathStr; reqGrp, reqRnk: Integer): Boolean;
var
  f: file;
  ok: Boolean;
begin
  Assign(f, path);
  {$I-}
  Reset(f, 1);
  {$I+}
  ok := (IOResult = 0);
  if not ok then begin
    OpenSrcFile := False;
    Exit;
  end;
  if srcDepth >= MAX_SRC then Die('require nesting too deep');
  if seenCount >= MAX_SEEN then Die('too many required files');
  seenFiles[seenCount] := path;
  Inc(seenCount);

  srcStack[srcDepth].fp := f;
  srcStack[srcDepth].bufPos := 0;
  srcStack[srcDepth].bufLen := 0;
  srcStack[srcDepth].pushback := -1;
  srcStack[srcDepth].path := path;
  srcStack[srcDepth].lineNum := 1;
  srcStack[srcDepth].reqGroup := reqGrp;
  srcStack[srcDepth].reqRank := reqRnk;
  Inc(srcDepth);
  OpenSrcFile := True;
end;

function SrcGetC(var sf: TSrcFile): Integer;
var
  nRead: Integer;
begin
  if sf.pushback >= 0 then begin
    SrcGetC := sf.pushback;
    sf.pushback := -1;
    Exit;
  end;
  if sf.bufPos >= sf.bufLen then begin
    BlockRead(sf.fp, sf.buf, SizeOf(sf.buf), nRead);
    sf.bufLen := nRead;
    sf.bufPos := 0;
    if sf.bufLen = 0 then begin
      SrcGetC := -1;
      Exit;
    end;
  end;
  SrcGetC := sf.buf[sf.bufPos];
  Inc(sf.bufPos);
end;

procedure SrcUngetC(var sf: TSrcFile; c: Integer);
begin
  sf.pushback := c;
end;

function IsSpaceChar(c: Integer): Boolean;
begin
  IsSpaceChar := (c = 32) or (c = 9) or (c = 10) or (c = 13) or (c = 11) or (c = 12);
end;

{ Advance past whitespace and '\' line comments; pops finished required files on EOF. }
function SkipWsAndComments: Integer;
var
  c, c2, curIdx: Integer;
begin
  while True do begin
    if srcDepth = 0 then begin
      SkipWsAndComments := -1;
      Exit;
    end;
    curIdx := srcDepth - 1;
    c := SrcGetC(srcStack[curIdx]);
    if c < 0 then begin
      Close(srcStack[curIdx].fp);
      Dec(srcDepth);
      if srcDepth > 0 then Continue;
      SkipWsAndComments := -1;
      Exit;
    end;
    if c = 10 then begin
      Inc(srcStack[curIdx].lineNum);
      Continue;
    end;
    if IsSpaceChar(c) then Continue;
    if c = Ord('\') then begin
      c2 := SrcGetC(srcStack[curIdx]);
      if (c2 < 0) or (c2 = 10) or IsSpaceChar(c2) then begin
        if c2 = 10 then
          Inc(srcStack[curIdx].lineNum)
        else if c2 >= 0 then begin
          repeat
            c := SrcGetC(srcStack[curIdx]);
          until (c < 0) or (c = 10);
          if c = 10 then Inc(srcStack[curIdx].lineNum);
        end;
        Continue;
      end;
      SrcUngetC(srcStack[curIdx], c2);
    end;
    SkipWsAndComments := c;
    Exit;
  end;
end;

function ReadQuoted(var sf: TSrcFile; const errMsg: string): TPathStr;
var
  c: Integer;
  s: TPathStr;
begin
  s := '';
  c := SrcGetC(sf);
  while (c >= 0) and (c <> 10) and (c <> Ord('"')) do begin
    s := s + Chr(c);
    c := SrcGetC(sf);
  end;
  if c <> Ord('"') then Die(errMsg);
  ReadQuoted := s;
end;

procedure ParseLineDirective(var sf: TSrcFile; lastC: Integer);
var
  c, newLine: Integer;
  hasDigits: Boolean;
begin
  c := lastC;
  newLine := 0;
  hasDigits := False;
  if (c = 10) or (c < 0) then Die('expected line number after ''#line''');
  while (c = 32) or (c = 9) do c := SrcGetC(sf);
  while (c >= Ord('0')) and (c <= Ord('9')) do begin
    newLine := newLine * 10 + (c - Ord('0'));
    hasDigits := True;
    c := SrcGetC(sf);
  end;
  if (not hasDigits) or (newLine <= 0) then Die('invalid line number in ''#line''');
  while (c = 32) or (c = 9) do c := SrcGetC(sf);
  if c = Ord('"') then begin
    sf.path := ReadQuoted(sf, 'unterminated filename in ''#line''');
    c := SrcGetC(sf);
  end;
  while (c >= 0) and (c <> 10) do c := SrcGetC(sf);
  sf.lineNum := newLine;
end;

function NextTok(var tok: TShortName): Boolean;
var
  c, curIdx: Integer;
begin
  while True do begin
    c := SkipWsAndComments;
    if c < 0 then begin
      tok := '';
      NextTok := False;
      Exit;
    end;
    curIdx := srcDepth - 1;
    tok := '';
    while (c >= 0) and (not IsSpaceChar(c)) do begin
      tok := tok + Chr(c);
      c := SrcGetC(srcStack[curIdx]);
    end;
    if tok = '#line' then begin
      ParseLineDirective(srcStack[curIdx], c);
      Continue;
    end;
    if c = 10 then Inc(srcStack[curIdx].lineNum);
    NextTok := True;
    Exit;
  end;
end;

function IsSeenFile(const path: TPathStr): Boolean;
var
  i: Integer;
begin
  for i := 0 to seenCount - 1 do
    if seenFiles[i] = path then begin
      IsSeenFile := True;
      Exit;
    end;
  IsSeenFile := False;
end;

function FileCanOpen(const path: TPathStr): Boolean;
var
  f: file;
  ok: Boolean;
begin
  Assign(f, path);
  {$I-}
  Reset(f, 1);
  {$I+}
  ok := (IOResult = 0);
  if ok then Close(f);
  FileCanOpen := ok;
end;

function ExtractDir(const path: TPathStr): TPathStr;
var
  i: Integer;
begin
  i := Length(path);
  while (i > 0) and (path[i] <> '/') do Dec(i);
  if i <= 0 then ExtractDir := ''
  else if i = 1 then ExtractDir := '/'
  else ExtractDir := Copy(path, 1, i - 1);
end;

function JoinPath(const dir, name: TPathStr): TPathStr;
begin
  if (dir = '') or ((Length(name) > 0) and (name[1] = '/')) then
    JoinPath := name
  else if dir[Length(dir)] = '/' then
    JoinPath := dir + name
  else
    JoinPath := dir + '/' + name;
end;

function ResolveRelPath(const curFile, rel: TPathStr): TPathStr;
var
  dir: TPathStr;
begin
  if (Length(rel) > 0) and (rel[1] = '/') then begin
    ResolveRelPath := rel;
    Exit;
  end;
  dir := ExtractDir(curFile);
  ResolveRelPath := JoinPath(dir, rel);
end;

{ Resolve 'require' candidates (virt -> rv32 -> generic) and push least-specific first onto the LIFO stack. }
procedure PushRequire(const name: TPathStr);
var
  cands: array[0..3] of TPathStr;
  nCands, c, i, grp: Integer;
  foundPaths: array[0..3] of TPathStr;
  foundRanks: array[0..3] of Integer;
  foundCnt, seenCnt: Integer;
  roots: array[0..MAX_ROOTS + 3] of TPathStr;
  rootCount, r: Integer;
  exeDir, p: TPathStr;
begin
  nCands := 0;
  if (Length(name) = 0) or (name[1] <> '/') then begin
    cands[nCands] := 'virt/' + name; Inc(nCands);
    cands[nCands] := 'rv32/' + name; Inc(nCands);
  end;
  cands[nCands] := name; Inc(nCands);

  foundCnt := 0;
  seenCnt := 0;

  if srcStack[srcDepth - 1].reqGroup = 0 then begin
    for c := 0 to nCands - 1 do begin
      p := ResolveRelPath(srcStack[srcDepth - 1].path, cands[c]);
      if IsSeenFile(p) then Inc(seenCnt)
      else if FileCanOpen(p) then begin
        foundPaths[foundCnt] := p;
        foundRanks[foundCnt] := c;
        Inc(foundCnt);
      end;
    end;
  end;

  if (foundCnt = 0) and (seenCnt = 0) and ((Length(name) = 0) or (name[1] <> '/')) then begin
    rootCount := 0;
    for r := 0 to extraRootCount - 1 do begin
      roots[rootCount] := extraLibRoots[r];
      Inc(rootCount);
    end;
    exeDir := ExtractDir(ParamStr(0));
    if exeDir <> '' then begin
      roots[rootCount] := JoinPath(exeDir, '../lib'); Inc(rootCount);
      roots[rootCount] := JoinPath(exeDir, 'lib'); Inc(rootCount);
    end;
    roots[rootCount] := 'lib'; Inc(rootCount);

    r := 0;
    while (r < rootCount) and (foundCnt = 0) and (seenCnt = 0) do begin
      for c := 0 to nCands - 1 do begin
        p := JoinPath(roots[r], cands[c]);
        if IsSeenFile(p) then Inc(seenCnt)
        else if FileCanOpen(p) then begin
          foundPaths[foundCnt] := p;
          foundRanks[foundCnt] := c;
          Inc(foundCnt);
        end;
      end;
      Inc(r);
    end;
  end;

  if (foundCnt = 0) and (seenCnt = 0) and (srcStack[srcDepth - 1].reqGroup <> 0) then begin
    for c := 0 to nCands - 1 do begin
      p := ResolveRelPath(srcStack[srcDepth - 1].path, cands[c]);
      if IsSeenFile(p) then Inc(seenCnt)
      else if FileCanOpen(p) then begin
        foundPaths[foundCnt] := p;
        foundRanks[foundCnt] := c;
        Inc(foundCnt);
      end;
    end;
  end;

  if foundCnt > 0 then begin
    grp := nextReqGroup;
    Inc(nextReqGroup);
    for i := foundCnt - 1 downto 0 do begin
      if not OpenSrcFile(foundPaths[i], grp, foundRanks[i]) then
        Die('cannot open required file ''' + foundPaths[i] + '''');
    end;
    Exit;
  end;
  if seenCnt > 0 then Exit;
  Die('cannot open required file ''' + name + '''');
end;

function ReadRequirePath: TPathStr;
var
  c: Integer;
begin
  c := SkipWsAndComments;
  if c <> Ord('"') then Die('expected quoted filename after ''require''');
  ReadRequirePath := ReadQuoted(srcStack[srcDepth - 1], 'unterminated filename in ''require''');
end;

{ Parse decimal, 0x/$ hex, or 0b binary integer literals. }
function ParseInt(const s: string; var outVal: LongInt): Boolean;
var
  p, code: Integer;
  neg: Boolean;
  sub: string;
  u: Int64;
begin
  if s = '' then begin ParseInt := False; Exit; end;
  p := 1;
  neg := False;
  if s[1] = '$' then begin
    if Length(s) = 1 then begin ParseInt := False; Exit; end;
    Val('$' + Copy(s, 2, Length(s) - 1), u, code);
    if code <> 0 then begin ParseInt := False; Exit; end;
    outVal := LongInt(u);
    ParseInt := True;
    Exit;
  end;
  if (s[1] = '-') or (s[1] = '+') then begin
    neg := (s[1] = '-');
    p := 2;
    if p > Length(s) then begin ParseInt := False; Exit; end;
  end;
  if (p + 1 <= Length(s)) and (s[p] = '0') and ((s[p + 1] = 'b') or (s[p + 1] = 'B')) then begin
    sub := Copy(s, p + 2, Length(s) - p - 1);
    if sub = '' then begin ParseInt := False; Exit; end;
    u := 0;
    for p := 1 to Length(sub) do begin
      if (sub[p] <> '0') and (sub[p] <> '1') then begin ParseInt := False; Exit; end;
      u := (u shl 1) or (Ord(sub[p]) - Ord('0'));
    end;
    if neg then u := -u;
    outVal := LongInt(u);
    ParseInt := True;
    Exit;
  end;
  if (p + 1 <= Length(s)) and (s[p] = '0') and ((s[p + 1] = 'x') or (s[p + 1] = 'X')) then begin
    Val('$' + Copy(s, p + 2, Length(s) - p - 1), u, code);
    if code <> 0 then begin ParseInt := False; Exit; end;
    if neg then u := -u;
    outVal := LongInt(u);
    ParseInt := True;
    Exit;
  end;
  if not (s[p] in ['0'..'9']) then begin ParseInt := False; Exit; end;
  Val(s, u, code);
  if code <> 0 then begin ParseInt := False; Exit; end;
  outVal := LongInt(u);
  ParseInt := True;
end;

function ParseFloat(const s: string; var outVal: Single): Boolean;
var
  p, code: Integer;
  d: Double;
  rest: string;
begin
  if s = '' then begin ParseFloat := False; Exit; end;
  p := 1;
  if (s[p] = '-') or (s[p] = '+') then Inc(p);
  if p > Length(s) then begin ParseFloat := False; Exit; end;
  if (p + 1 <= Length(s)) and (s[p] = '0') and ((s[p + 1] = 'x') or (s[p + 1] = 'X')) then begin
    ParseFloat := False; Exit;
  end;
  if not ((s[p] in ['0'..'9']) or (s[p] = '.')) then begin ParseFloat := False; Exit; end;
  if (s[p] = '.') and ((p + 1 > Length(s)) or not (s[p + 1] in ['0'..'9'])) then begin
    ParseFloat := False; Exit;
  end;
  rest := Copy(s, p, Length(s) - p + 1);
  if (Pos('.', rest) = 0) and (Pos('e', rest) = 0) and (Pos('E', rest) = 0) then begin
    ParseFloat := False; Exit;
  end;
  Val(s, d, code);
  if code <> 0 then begin ParseFloat := False; Exit; end;
  outVal := Single(d);
  ParseFloat := True;
end;

{ --- Symbol & Type Stack Helpers --- }

function FindFunc(const name: TShortName): Integer;
var
  i: Integer;
begin
  for i := 0 to funcCount - 1 do
    if funcs[i].name = name then begin FindFunc := i; Exit; end;
  FindFunc := -1;
end;

function FindBuffer(const name: TShortName): Integer;
var
  i: Integer;
begin
  for i := 0 to bufferCount - 1 do
    if buffers[i].name = name then begin FindBuffer := i; Exit; end;
  FindBuffer := -1;
end;

function FindConst(const name: TShortName): Integer;
var
  i: Integer;
begin
  for i := 0 to constCount - 1 do
    if consts[i].name = name then begin FindConst := i; Exit; end;
  FindConst := -1;
end;

function FindLocal(const name: TShortName): Integer;
var
  i: Integer;
begin
  for i := 0 to localCount - 1 do
    if locals[i].name = name then begin FindLocal := i; Exit; end;
  FindLocal := -1;
end;

procedure TPush(t: TStackType);
begin
  if tstackLen >= MAX_TSTACK then Die('type stack overflow');
  tstack[tstackLen] := t;
  Inc(tstackLen);
end;

function TPop: TStackType;
begin
  if tstackLen = 0 then Die('type stack underflow');
  Dec(tstackLen);
  TPop := tstack[tstackLen];
end;

procedure TPopExpect(expected: TStackType);
var
  got: TStackType;
begin
  got := TPop;
  if got <> expected then
    Die('expected ' + TypeName(expected) + ', got ' + TypeName(got));
end;

procedure CheckReturnStack;
var
  i: Integer;
begin
  if tstackLen <> curOutCount then Die('return stack depth mismatch');
  for i := 0 to curOutCount - 1 do
    if tstack[i] <> curOuts[i] then Die('return stack type mismatch');
end;

procedure CopyStack(var dst: TTypeArray; var dstLen: Integer;
                    const src: TTypeArray; srcLen: Integer);
var
  i: Integer;
begin
  dstLen := srcLen;
  for i := 0 to srcLen - 1 do
    dst[i] := src[i];
end;

function StackEq(const a: TTypeArray; aLen: Integer;
                 const b: TTypeArray; bLen: Integer): Boolean;
var
  i: Integer;
begin
  if aLen <> bLen then begin StackEq := False; Exit; end;
  for i := 0 to aLen - 1 do
    if a[i] <> b[i] then begin StackEq := False; Exit; end;
  StackEq := True;
end;

function SigsMatch(const a, b: TFuncSig): Boolean;
var
  i: Integer;
begin
  if (a.inCount <> b.inCount) or (a.outCount <> b.outCount) then begin
    SigsMatch := False; Exit;
  end;
  for i := 0 to a.inCount - 1 do
    if a.inTypes[i] <> b.inTypes[i] then begin SigsMatch := False; Exit; end;
  for i := 0 to a.outCount - 1 do
    if a.outTypes[i] <> b.outTypes[i] then begin SigsMatch := False; Exit; end;
  SigsMatch := True;
end;

function AllocFunc: Integer;
begin
  if funcCount >= MAX_FUNCS then Die('too many functions');
  AllocFunc := funcCount;
  FillChar(funcs[funcCount], SizeOf(TFuncSig), 0);
  Inc(funcCount);
end;

procedure AddBuiltinConst(const name: TShortName; val: LongInt);
begin
  if constCount >= MAX_CONSTS then Die('too many constants');
  consts[constCount].name := name;
  consts[constCount].ty := tyW;
  consts[constCount].wVal := val;
  consts[constCount].fVal := 0;
  consts[constCount].isDefault := False;
  consts[constCount].reqGroup := 0;
  consts[constCount].reqRank := 0;
  Inc(constCount);
end;

{ --- RV32 Code Emission Helpers --- }

procedure EmitPushW(w: LongInt);
begin
  RvLi(RV_T0, Cardinal(w));
  PushReg(RV_T0);
end;

procedure EmitPushData(off: Cardinal);
begin
  if off <= 2047 then
    RvAddi(RV_T0, RV_DB, LongInt(off))
  else begin
    RvLi(RV_T0, off);
    Emit32(EncR($00, RV_T0, RV_DB, 0, RV_T0, $33));
  end;
  PushReg(RV_T0);
end;

{ Emit LUI+ADDI placeholder for s2-relative __heap_start; patched by PatchForwardRefs. }
procedure EmitPushHeapStart;
begin
  if heapRelocCount >= MAX_HRELOCS then Die('too many __heap_start references');
  heapRelocs[heapRelocCount] := CodePC;
  Inc(heapRelocCount);
  Emit32(EncU(0, RV_T0, $37));
  RvAddi(RV_T0, RV_T0, 0);
  Emit32(EncR($00, RV_T0, RV_DB, 0, RV_T0, $33));
  PushReg(RV_T0);
end;

{ Emit SLT/SLTU comparison returning IR4(th) boolean (-1 or 0). }
procedure EmitCmp(f3: Cardinal; swapOps, invert: Boolean);
begin
  if swapOps then begin
    PopReg(RV_T0);
    PopReg(RV_T1);
  end else begin
    PopReg(RV_T1);
    PopReg(RV_T0);
  end;
  Emit32(EncR($00, RV_T1, RV_T0, f3, RV_T0, $33));
  if invert then Emit32(EncI(1, RV_T0, 4, RV_T0, $13));
  Emit32(EncR($20, RV_T0, RV_ZERO, 0, RV_T0, $33));
  PushReg(RV_T0);
end;

{ Emit equality/inequality comparison via SUB + SLTIU/SLTU returning -1 or 0. }
procedure EmitEqNe(isNe: Boolean);
begin
  PopReg(RV_T1);
  PopReg(RV_T0);
  Emit32(EncR($20, RV_T1, RV_T0, 0, RV_T0, $33));
  if isNe then
    Emit32(EncR($00, RV_T0, RV_ZERO, 3, RV_T0, $33))
  else
    Emit32(EncI(1, RV_T0, 3, RV_T0, $13));
  Emit32(EncR($20, RV_T0, RV_ZERO, 0, RV_T0, $33));
  PushReg(RV_T0);
end;

procedure EmitLocalAddr(slot: Integer);
var
  off: LongInt;
begin
  off := locals[slot].fpOff;
  if (off >= -2048) and (off <= 2047) then
    RvAddi(RV_T0, RV_FP, off)
  else begin
    RvLi(RV_T0, Cardinal(off));
    Emit32(EncR($00, RV_T0, RV_FP, 0, RV_T0, $33));
  end;
  PushReg(RV_T0);
end;

{ Load or store a local variable relative to fp, using t1 as scratch for large offsets. }
procedure EmitLocalMem(isStore: Boolean; reg: Cardinal; off: LongInt);
begin
  if (off >= -2048) and (off <= 2047) then begin
    if isStore then RvSw(reg, off, RV_FP) else RvLw(reg, off, RV_FP);
  end else begin
    RvLi(RV_T1, Cardinal(off));
    Emit32(EncR($00, RV_T1, RV_FP, 0, RV_T1, $33));
    if isStore then RvSw(reg, 0, RV_T1) else RvLw(reg, 0, RV_T1);
  end;
end;

procedure EmitLocalGet(slot: Integer);
begin
  EmitLocalMem(False, RV_T0, locals[slot].fpOff);
  PushReg(RV_T0);
end;

procedure EmitLocalSet(slot: Integer);
begin
  PopReg(RV_T0);
  EmitLocalMem(True, RV_T0, locals[slot].fpOff);
end;

procedure EmitFnEpilogue;
begin
  RvAddi(RV_SP, RV_FP, 0);
  RvLw(RV_FP, 0, RV_SP);
  RvLw(RV_RA, 4, RV_SP);
  RvAddi(RV_SP, RV_SP, 8);
  Emit32(EncI(0, RV_RA, 0, RV_ZERO, $67));
end;

{ --- Built-in Words (table-driven for ALU, comparisons, FP, and memory) --- }

type
  TAluOpDef = record
    name: string[11];
    in1, in2, outTy: TStackType;
    f7, f3: Byte;
  end;

  TCmpOpDef = record
    name: string[7];
    inTy: TStackType;
    isEqNe, isNeOrInv, swapOps: Boolean;
    f3: Byte;
  end;

  TFpOpDef = record
    name: string[7];
    code: Byte;
    swapOps, invert: Boolean;
  end;

  TMemOpDef = record
    name: string[5];
    ty: TStackType;
    f3: Byte;
  end;

const
  ALU_OPS: array[0..28] of TAluOpDef = (
    (name: '+';      in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 0),
    (name: 'a+';     in1: tyA;   in2: tyW;   outTy: tyA;   f7: $00; f3: 0),
    (name: '-';      in1: tyW;   in2: tyW;   outTy: tyW;   f7: $20; f3: 0),
    (name: 'a-';     in1: tyA;   in2: tyW;   outTy: tyA;   f7: $20; f3: 0),
    (name: 'adiff';  in1: tyA;   in2: tyA;   outTy: tyW;   f7: $20; f3: 0),
    (name: '*';      in1: tyW;   in2: tyW;   outTy: tyW;   f7: $01; f3: 0),
    (name: '/';      in1: tyW;   in2: tyW;   outTy: tyW;   f7: $01; f3: 4),
    (name: 'mod';    in1: tyW;   in2: tyW;   outTy: tyW;   f7: $01; f3: 6),
    (name: 'u/';     in1: tyW;   in2: tyW;   outTy: tyW;   f7: $01; f3: 5),
    (name: 'umod';   in1: tyW;   in2: tyW;   outTy: tyW;   f7: $01; f3: 7),
    (name: 'and';    in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 7),
    (name: 'or';     in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 6),
    (name: 'xor';    in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 4),
    (name: '<<';     in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 1),
    (name: 'u>>';    in1: tyW;   in2: tyW;   outTy: tyW;   f7: $00; f3: 5),
    (name: '>>';     in1: tyW;   in2: tyW;   outTy: tyW;   f7: $20; f3: 5),
    (name: 'i32+';   in1: tyI32; in2: tyI32; outTy: tyI32; f7: $00; f3: 0),
    (name: 'i32-';   in1: tyI32; in2: tyI32; outTy: tyI32; f7: $20; f3: 0),
    (name: 'i32*';   in1: tyI32; in2: tyI32; outTy: tyI32; f7: $01; f3: 0),
    (name: 'i32/';   in1: tyI32; in2: tyI32; outTy: tyI32; f7: $01; f3: 4),
    (name: 'i32mod'; in1: tyI32; in2: tyI32; outTy: tyI32; f7: $01; f3: 6),
    (name: 'u32/';   in1: tyI32; in2: tyI32; outTy: tyI32; f7: $01; f3: 5),
    (name: 'u32mod'; in1: tyI32; in2: tyI32; outTy: tyI32; f7: $01; f3: 7),
    (name: 'i32and'; in1: tyI32; in2: tyI32; outTy: tyI32; f7: $00; f3: 7),
    (name: 'i32or';  in1: tyI32; in2: tyI32; outTy: tyI32; f7: $00; f3: 6),
    (name: 'i32xor'; in1: tyI32; in2: tyI32; outTy: tyI32; f7: $00; f3: 4),
    (name: 'i32<<';  in1: tyI32; in2: tyW;   outTy: tyI32; f7: $00; f3: 1),
    (name: 'u32>>';  in1: tyI32; in2: tyW;   outTy: tyI32; f7: $00; f3: 5),
    (name: 'i32>>';  in1: tyI32; in2: tyW;   outTy: tyI32; f7: $20; f3: 5)
  );

  CMP_OPS: array[0..25] of TCmpOpDef = (
    (name: '=';     inTy: tyW;   isEqNe: True;  isNeOrInv: False; swapOps: False; f3: 0),
    (name: 'a=';    inTy: tyA;   isEqNe: True;  isNeOrInv: False; swapOps: False; f3: 0),
    (name: 'i32=';  inTy: tyI32; isEqNe: True;  isNeOrInv: False; swapOps: False; f3: 0),
    (name: '<>';    inTy: tyW;   isEqNe: True;  isNeOrInv: True;  swapOps: False; f3: 0),
    (name: 'a<>';   inTy: tyA;   isEqNe: True;  isNeOrInv: True;  swapOps: False; f3: 0),
    (name: 'i32<>'; inTy: tyI32; isEqNe: True;  isNeOrInv: True;  swapOps: False; f3: 0),
    (name: '<';     inTy: tyW;   isEqNe: False; isNeOrInv: False; swapOps: False; f3: 2),
    (name: 'i32<';  inTy: tyI32; isEqNe: False; isNeOrInv: False; swapOps: False; f3: 2),
    (name: '>';     inTy: tyW;   isEqNe: False; isNeOrInv: False; swapOps: True;  f3: 2),
    (name: 'i32>';  inTy: tyI32; isEqNe: False; isNeOrInv: False; swapOps: True;  f3: 2),
    (name: '<=';    inTy: tyW;   isEqNe: False; isNeOrInv: True;  swapOps: True;  f3: 2),
    (name: 'i32<='; inTy: tyI32; isEqNe: False; isNeOrInv: True;  swapOps: True;  f3: 2),
    (name: '>=';    inTy: tyW;   isEqNe: False; isNeOrInv: True;  swapOps: False; f3: 2),
    (name: 'i32>='; inTy: tyI32; isEqNe: False; isNeOrInv: True;  swapOps: False; f3: 2),
    (name: 'u<';    inTy: tyW;   isEqNe: False; isNeOrInv: False; swapOps: False; f3: 3),
    (name: 'a<';    inTy: tyA;   isEqNe: False; isNeOrInv: False; swapOps: False; f3: 3),
    (name: 'u32<';  inTy: tyI32; isEqNe: False; isNeOrInv: False; swapOps: False; f3: 3),
    (name: 'u>';    inTy: tyW;   isEqNe: False; isNeOrInv: False; swapOps: True;  f3: 3),
    (name: 'a>';    inTy: tyA;   isEqNe: False; isNeOrInv: False; swapOps: True;  f3: 3),
    (name: 'u32>';  inTy: tyI32; isEqNe: False; isNeOrInv: False; swapOps: True;  f3: 3),
    (name: 'u<=';   inTy: tyW;   isEqNe: False; isNeOrInv: True;  swapOps: True;  f3: 3),
    (name: 'a<=';   inTy: tyA;   isEqNe: False; isNeOrInv: True;  swapOps: True;  f3: 3),
    (name: 'u32<='; inTy: tyI32; isEqNe: False; isNeOrInv: True;  swapOps: True;  f3: 3),
    (name: 'u>=';   inTy: tyW;   isEqNe: False; isNeOrInv: True;  swapOps: False; f3: 3),
    (name: 'a>=';   inTy: tyA;   isEqNe: False; isNeOrInv: True;  swapOps: False; f3: 3),
    (name: 'u32>='; inTy: tyI32; isEqNe: False; isNeOrInv: True;  swapOps: False; f3: 3)
  );

  FP_BIN_OPS: array[0..3] of TFpOpDef = (
    (name: 'f+'; code: $00; swapOps: False; invert: False),
    (name: 'f-'; code: $04; swapOps: False; invert: False),
    (name: 'f*'; code: $08; swapOps: False; invert: False),
    (name: 'f/'; code: $0C; swapOps: False; invert: False)
  );

  FP_CMP_OPS: array[0..5] of TFpOpDef = (
    (name: 'f=';  code: 2; swapOps: False; invert: False),
    (name: 'f<>'; code: 2; swapOps: False; invert: True),
    (name: 'f<';  code: 1; swapOps: False; invert: False),
    (name: 'f>';  code: 1; swapOps: True;  invert: False),
    (name: 'f<='; code: 0; swapOps: False; invert: False),
    (name: 'f>='; code: 0; swapOps: True;  invert: False)
  );

  LOAD_OPS: array[0..7] of TMemOpDef = (
    (name: 'u8@';  ty: tyW;   f3: 4),
    (name: 'i8@';  ty: tyW;   f3: 0),
    (name: 'u16@'; ty: tyW;   f3: 5),
    (name: 'i16@'; ty: tyW;   f3: 1),
    (name: '@';    ty: tyW;   f3: 2),
    (name: 'a@';   ty: tyA;   f3: 2),
    (name: 'f@';   ty: tyF;   f3: 2),
    (name: 'i32@'; ty: tyI32; f3: 2)
  );

  STORE_OPS: array[0..5] of TMemOpDef = (
    (name: 'i8!';  ty: tyW;   f3: 0),
    (name: 'i16!'; ty: tyW;   f3: 1),
    (name: '!';    ty: tyW;   f3: 2),
    (name: 'a!';   ty: tyA;   f3: 2),
    (name: 'f!';   ty: tyF;   f3: 2),
    (name: 'i32!'; ty: tyI32; f3: 2)
  );

{ Match and emit RV32 code for built-in stack, ALU, comparison, FP, and memory words. }
function HandleBuiltin(const tok: TShortName): Boolean;
var
  i: Integer;
  t1, t2, t3: TStackType;
begin
  HandleBuiltin := True;

  for i := 0 to High(ALU_OPS) do
    if tok = ALU_OPS[i].name then begin
      TPopExpect(ALU_OPS[i].in2);
      TPopExpect(ALU_OPS[i].in1);
      TPush(ALU_OPS[i].outTy);
      PopReg(RV_T1);
      PopReg(RV_T0);
      Emit32(EncR(ALU_OPS[i].f7, RV_T1, RV_T0, ALU_OPS[i].f3, RV_T0, $33));
      PushReg(RV_T0);
      Exit;
    end;

  for i := 0 to High(CMP_OPS) do
    if tok = CMP_OPS[i].name then begin
      TPopExpect(CMP_OPS[i].inTy);
      TPopExpect(CMP_OPS[i].inTy);
      TPush(tyW);
      if CMP_OPS[i].isEqNe then EmitEqNe(CMP_OPS[i].isNeOrInv)
      else EmitCmp(CMP_OPS[i].f3, CMP_OPS[i].swapOps, CMP_OPS[i].isNeOrInv);
      Exit;
    end;

  for i := 0 to High(FP_BIN_OPS) do
    if tok = FP_BIN_OPS[i].name then begin
      TPopExpect(tyF); TPopExpect(tyF); TPush(tyF);
      PopF(RV_FT1); PopF(RV_FT0);
      Emit32(EncR(FP_BIN_OPS[i].code, RV_FT1, RV_FT0, 7, RV_FT0, $53));
      PushF(RV_FT0);
      Exit;
    end;

  for i := 0 to High(FP_CMP_OPS) do
    if tok = FP_CMP_OPS[i].name then begin
      TPopExpect(tyF); TPopExpect(tyF); TPush(tyW);
      PopF(RV_FT1); PopF(RV_FT0);
      if FP_CMP_OPS[i].swapOps then
        Emit32(EncR($50, RV_FT0, RV_FT1, FP_CMP_OPS[i].code, RV_T0, $53))
      else
        Emit32(EncR($50, RV_FT1, RV_FT0, FP_CMP_OPS[i].code, RV_T0, $53));
      if FP_CMP_OPS[i].invert then
        Emit32(EncI(1, RV_T0, 4, RV_T0, $13));
      Emit32(EncR($20, RV_T0, RV_ZERO, 0, RV_T0, $33));
      PushReg(RV_T0);
      Exit;
    end;

  for i := 0 to High(LOAD_OPS) do
    if tok = LOAD_OPS[i].name then begin
      TPopExpect(tyA);
      TPush(LOAD_OPS[i].ty);
      PopReg(RV_T0);
      Emit32(EncI(0, RV_T0, LOAD_OPS[i].f3, RV_T0, $03));
      PushReg(RV_T0);
      Exit;
    end;

  for i := 0 to High(STORE_OPS) do
    if tok = STORE_OPS[i].name then begin
      TPopExpect(tyA);
      TPopExpect(STORE_OPS[i].ty);
      PopReg(RV_T0);
      PopReg(RV_T1);
      Emit32(EncS(0, RV_T1, RV_T0, STORE_OPS[i].f3));
      Exit;
    end;

  if (tok = 'invert') or (tok = 'i32invert') then begin
    if tok = 'invert' then t1 := tyW else t1 := tyI32;
    TPopExpect(t1); TPush(t1);
    PopReg(RV_T0);
    Emit32(EncI(-1, RV_T0, 4, RV_T0, $13));
    PushReg(RV_T0);
  end else if (tok = 'neg') or (tok = 'i32neg') then begin
    if tok = 'neg' then t1 := tyW else t1 := tyI32;
    TPopExpect(t1); TPush(t1);
    PopReg(RV_T0);
    Emit32(EncR($20, RV_T0, RV_ZERO, 0, RV_T0, $33));
    PushReg(RV_T0);
  end else if tok = 'fnegate' then begin
    TPopExpect(tyF); TPush(tyF);
    PopF(RV_FT0);
    Emit32(EncR($10, RV_FT0, RV_FT0, 1, RV_FT0, $53));
    PushF(RV_FT0);

  end else if tok = 'i>a' then begin
    TPopExpect(tyW); TPush(tyA);
  end else if tok = 'a>i' then begin
    TPopExpect(tyA); TPush(tyW);
  end else if (tok = 'i>i32') or (tok = 'u>i32') then begin
    TPopExpect(tyW); TPush(tyI32);
  end else if tok = 'i32>i' then begin
    TPopExpect(tyI32); TPush(tyW);
  end else if tok = 'i>f' then begin
    TPopExpect(tyW); TPush(tyF);
    PopReg(RV_T0);
    Emit32(EncR($68, 0, RV_T0, 7, RV_FT0, $53)); { FCVT.S.W }
    PushF(RV_FT0);
  end else if tok = 'f>i' then begin
    TPopExpect(tyF); TPush(tyW);
    PopF(RV_FT0);
    Emit32(EncR($60, 0, RV_FT0, 1, RV_T0, $53)); { FCVT.W.S (RTZ) }
    PushReg(RV_T0);
  end else if tok = 'sext' then begin
    TPopExpect(tyW); TPush(tyW);
    PopReg(RV_T0);
    Emit32(EncI(24, RV_T0, 1, RV_T0, $13));
    Emit32(EncI($400 or 24, RV_T0, 5, RV_T0, $13));
    PushReg(RV_T0);

  end else if tok = 'dup' then begin
    t1 := TPop; TPush(t1); TPush(t1);
    RvLw(RV_T0, 0, RV_DSP); PushReg(RV_T0);
  end else if tok = 'drop' then begin
    TPop;
    RvAddi(RV_DSP, RV_DSP, 4);
  end else if tok = 'swap' then begin
    t2 := TPop; t1 := TPop; TPush(t2); TPush(t1);
    RvLw(RV_T0, 0, RV_DSP); RvLw(RV_T1, 4, RV_DSP);
    RvSw(RV_T1, 0, RV_DSP); RvSw(RV_T0, 4, RV_DSP);
  end else if tok = 'over' then begin
    t2 := TPop; t1 := TPop; TPush(t1); TPush(t2); TPush(t1);
    RvLw(RV_T0, 4, RV_DSP); PushReg(RV_T0);
  end else if tok = 'rot' then begin
    t3 := TPop; t2 := TPop; t1 := TPop; TPush(t2); TPush(t3); TPush(t1);
    RvLw(RV_T2, 0, RV_DSP); RvLw(RV_T1, 4, RV_DSP); RvLw(RV_T0, 8, RV_DSP);
    RvSw(RV_T1, 8, RV_DSP); RvSw(RV_T2, 4, RV_DSP); RvSw(RV_T0, 0, RV_DSP);
  end else if tok = 'nip' then begin
    t2 := TPop; TPop; TPush(t2);
    PopReg(RV_T0); RvSw(RV_T0, 0, RV_DSP);
  end else if tok = 'tuck' then begin
    t2 := TPop; t1 := TPop; TPush(t2); TPush(t1); TPush(t2);
    RvLw(RV_T1, 0, RV_DSP); RvLw(RV_T0, 4, RV_DSP);
    RvSw(RV_T1, 4, RV_DSP); RvSw(RV_T0, 0, RV_DSP);
    PushReg(RV_T1);
  end else if tok = 'pick' then begin
    TPopExpect(tyW); TPush(tyW);
    PopReg(RV_T0);
    Emit32(EncI(2, RV_T0, 1, RV_T0, $13));
    Emit32(EncR($00, RV_T0, RV_DSP, 0, RV_T0, $33));
    RvLw(RV_T0, 0, RV_T0);
    PushReg(RV_T0);
  end else
    HandleBuiltin := False;
end;

{ --- Signatures, Locals, Control Flow & Word Body --- }

{ Parse '( inputs -> outputs )' signature; optionally records named input parameters as locals. }
procedure ParseSigBody(var sig: TFuncSig; recordLocals: Boolean);
var
  tok, prevName: TShortName;
  inOutputs, hasNames, expectType: Boolean;
  ty: TStackType;
  byteSize: LongInt;
begin
  inOutputs := False;
  hasNames := False;
  expectType := False;
  prevName := '';
  while NextTok(tok) do begin
    if tok = ')' then begin
      if expectType then Die('expected type after parameter name');
      if inOutputs and (sig.outCount = 0) then Die('expected output type after ''->''');
      Exit;
    end;
    if tok = '->' then begin
      if inOutputs then Die('duplicate ''->'' in signature');
      if expectType then Die('expected type after parameter name');
      inOutputs := True;
      Continue;
    end;
    if inOutputs then begin
      ty := ParseTypeTok(tok);
      if ty = tyNone then Die('expected output type in signature');
      if sig.outCount >= MAX_OUTS then Die('too many outputs');
      sig.outTypes[sig.outCount] := ty;
      Inc(sig.outCount);
    end else if tok = ':bytes' then begin
      if not expectType then Die('expected parameter name before '':bytes''');
      if not NextTok(tok) or not ParseInt(tok, byteSize) or (byteSize <= 0) then
        Die('invalid byte count after '':bytes''');
      if sig.inCount >= MAX_PARAMS then Die('too many parameters');
      sig.inTypes[sig.inCount] := tyA;
      sig.inNames[sig.inCount] := prevName;
      Inc(sig.inCount);
      if recordLocals then begin
        if localCount >= MAX_LOCALS then Die('too many locals');
        locals[localCount].name := prevName;
        locals[localCount].ty := tyA;
        locals[localCount].size := Cardinal(byteSize);
        Inc(localCount);
      end;
      expectType := False;
    end else begin
      ty := ParseTypeTok(tok);
      if ty <> tyNone then begin
        if (sig.inCount > 0) and (not expectType) and hasNames then
          Die('cannot mix named and unnamed input parameters');
        if sig.inCount >= MAX_PARAMS then Die('too many parameters');
        sig.inTypes[sig.inCount] := ty;
        if expectType then begin
          sig.inNames[sig.inCount] := prevName;
          if recordLocals then begin
            if localCount >= MAX_LOCALS then Die('too many locals');
            locals[localCount].name := prevName;
            locals[localCount].ty := ty;
            locals[localCount].size := 0;
            Inc(localCount);
          end;
        end else
          sig.inNames[sig.inCount] := '';
        Inc(sig.inCount);
        expectType := False;
      end else begin
        if expectType then Die('expected type after parameter name');
        if (sig.inCount > 0) and (not hasNames) then
          Die('cannot mix named and unnamed input parameters');
        hasNames := True;
        prevName := tok;
        expectType := True;
      end;
    end;
  end;
  Die('unexpected EOF inside signature');
end;

function FindOrAddLabel(const name: TShortName): Integer;
var
  i: Integer;
begin
  for i := 0 to labelCount - 1 do
    if labels[i].name = name then begin FindOrAddLabel := i; Exit; end;
  if labelCount >= MAX_LABELS then Die('too many labels');
  FindOrAddLabel := labelCount;
  labels[labelCount].name := name;
  labels[labelCount].defined := False;
  labels[labelCount].addr := 0;
  Inc(labelCount);
end;

function ParseLocalVar(allowBytes: Boolean): Integer;
var
  name, tok: TShortName;
  ty: TStackType;
  sz: LongInt;
begin
  if not NextTok(name) then Die('expected local variable name after ''var''');
  if FindLocal(name) >= 0 then Die('duplicate local variable name');
  if not NextTok(tok) then Die('expected type after local variable name');
  if localCount >= MAX_LOCALS then Die('too many locals');
  ParseLocalVar := localCount;
  locals[localCount].name := name;
  if tok = ':bytes' then begin
    if not allowBytes then Die('cannot assign '':bytes'' local with ''->''');
    if not NextTok(tok) or not ParseInt(tok, sz) or (sz <= 0) then
      Die('invalid byte count after '':bytes''');
    locals[localCount].ty := tyA;
    locals[localCount].size := Cardinal(sz);
  end else begin
    ty := ParseTypeTok(tok);
    if ty = tyNone then Die('expected '':i'', '':a'', '':f'', or '':i32'' for local variable');
    locals[localCount].ty := ty;
    locals[localCount].size := 0;
  end;
  locals[localCount].fpOff := 0;
  Inc(localCount);
end;

procedure EmitGlobalRead(idx: Integer);
begin
  if buffers[idx].ty = tyNone then begin
    TPush(tyA);
    EmitPushData(buffers[idx].offset);
  end else begin
    TPush(buffers[idx].ty);
    EmitPushData(buffers[idx].offset);
    PopReg(RV_T0);
    RvLw(RV_T0, 0, RV_T0);
    PushReg(RV_T0);
  end;
end;

procedure EmitGlobalStore;
begin
  PopReg(RV_T1);
  PopReg(RV_T0);
  RvSw(RV_T0, 0, RV_T1);
end;

{ Handle '&' (address-of) and '+->' (add-assign) for locals, globals, and function pointers. }
function HandleVarOp(const tok: TShortName): Boolean;
var
  ident: TShortName;
  idx: Integer;
  dt: TStackType;
begin
  HandleVarOp := True;
  if tok = '&' then begin
    if not NextTok(ident) then Die('expected identifier after ''&''');
    idx := FindLocal(ident);
    if idx >= 0 then begin
      if locals[idx].size > 0 then Die(''':bytes'' local is already an address');
      EmitLocalAddr(idx);
      TPush(tyA);
      Exit;
    end;
    idx := FindBuffer(ident);
    if idx >= 0 then begin
      if buffers[idx].ty = tyNone then Die(''':bytes'' or ''data'' symbol is already an address');
      EmitPushData(buffers[idx].offset);
      TPush(tyA);
      Exit;
    end;
    idx := FindFunc(ident);
    if idx >= 0 then begin
      if funcs[idx].isExtern then Die('cannot take address of ''extern'' function');
      if funcs[idx].hasAddr then begin
        Emit32(EncU(0, RV_T0, $17));
        RvAddi(RV_T0, RV_T0, LongInt(funcs[idx].addr) - LongInt(CodePC - 4));
      end else begin
        if fwdCallCount >= MAX_FWDCALLS then Die('too many forward calls');
        fwdCalls[fwdCallCount].patchOff := CodePC;
        fwdCalls[fwdCallCount].funcIdx := idx;
        fwdCalls[fwdCallCount].isFnPtr := True;
        Inc(fwdCallCount);
        Emit32(EncU(0, RV_T0, $17));
        RvAddi(RV_T0, RV_T0, 0);
      end;
      PushReg(RV_T0);
      TPush(tyA);
      Exit;
    end;
    Die('unknown variable or function in ''&''');
  end else if tok = '+->' then begin
    if not NextTok(ident) then Die('expected identifier after ''+->''');
    idx := FindLocal(ident);
    if idx >= 0 then begin
      if locals[idx].size > 0 then Die('cannot ''+->'' to '':bytes'' local');
      if locals[idx].ty = tyA then dt := tyW else dt := locals[idx].ty;
      TPopExpect(dt);
      EmitLocalGet(idx);
      if locals[idx].ty = tyF then begin
        PopF(RV_FT0); PopF(RV_FT1);
        Emit32(EncR($00, RV_FT1, RV_FT0, 7, RV_FT0, $53));
        PushF(RV_FT0);
      end else begin
        PopReg(RV_T0); PopReg(RV_T1);
        Emit32(EncR($00, RV_T1, RV_T0, 0, RV_T0, $33));
        PushReg(RV_T0);
      end;
      EmitLocalSet(idx);
      Exit;
    end;
    idx := FindBuffer(ident);
    if idx < 0 then Die('unknown variable in ''+->''');
    if buffers[idx].ty = tyNone then Die('cannot ''+->'' to '':bytes'' or ''data'' symbol');
    if buffers[idx].ty = tyA then dt := tyW else dt := buffers[idx].ty;
    TPopExpect(dt);
    EmitPushData(buffers[idx].offset);
    PopReg(RV_T2);
    if buffers[idx].ty = tyF then begin
      PopF(RV_FT1);
      Emit32(EncI(0, RV_T2, 2, RV_FT0, $07)); { FLW ft0, 0(t2) }
      Emit32(EncR($00, RV_FT1, RV_FT0, 7, RV_FT0, $53));
      Emit32((EncS(0, RV_FT0, RV_T2, 2) and not Cardinal($7F)) or $27);
    end else begin
      PopReg(RV_T1);
      RvLw(RV_T0, 0, RV_T2);
      Emit32(EncR($00, RV_T1, RV_T0, 0, RV_T0, $33));
      RvSw(RV_T0, 0, RV_T2);
    end;
  end else
    HandleVarOp := False;
end;

{ Handle structured control flow (if/else/then, begin/while/repeat, return) and labels (::/jmp/jz/jtab). }
function HandleControlFlow(const tok: TShortName; var unreachable: Boolean): Boolean;
var
  bi, lidx, cnt, k: Integer;
  lblName: TShortName;
  jtabList: array[0..MAX_JTAB - 1] of Integer;
begin
  HandleControlFlow := True;
  if tok = 'return' then begin
    if not unreachable then begin
      CheckReturnStack;
      EmitFnEpilogue;
      unreachable := True;
    end;
  end else if tok = 'if' then begin
    TPopExpect(tyW);
    if blockCount >= MAX_BLOCKS then Die('too many nested blocks');
    bi := blockCount;
    Inc(blockCount);
    blocks[bi].kind := bkIf;
    blocks[bi].hasElse := False;
    blocks[bi].savedUnreach := unreachable;
    blocks[bi].ifUnreach := False;
    CopyStack(blocks[bi].savedStack, blocks[bi].savedLen, tstack, tstackLen);
    PopReg(RV_T0);
    blocks[bi].patchElse := CodePC;
    Emit32(0);
    Emit32(0);
  end else if tok = 'else' then begin
    if (blockCount = 0) or (blocks[blockCount - 1].kind <> bkIf) then
      Die('''else'' without ''if''');
    bi := blockCount - 1;
    blocks[bi].hasElse := True;
    blocks[bi].ifUnreach := unreachable;
    CopyStack(blocks[bi].elseStack, blocks[bi].elseLen, tstack, tstackLen);
    CopyStack(tstack, tstackLen, blocks[bi].savedStack, blocks[bi].savedLen);
    unreachable := blocks[bi].savedUnreach;
    blocks[bi].patchEnd := CodePC;
    Emit32(0);
    Patch32(blocks[bi].patchElse, EncB(8, RV_ZERO, RV_T0, 1));
    Patch32(blocks[bi].patchElse + 4,
            EncJ(LongInt(CodePC) - LongInt(blocks[bi].patchElse + 4), RV_ZERO));
  end else if tok = 'then' then begin
    if (blockCount = 0) or (blocks[blockCount - 1].kind <> bkIf) then
      Die('''then'' without ''if''');
    bi := blockCount - 1;
    if not blocks[bi].hasElse then begin
      if (not unreachable) and
         (not StackEq(tstack, tstackLen, blocks[bi].savedStack, blocks[bi].savedLen)) then
        Die('unbalanced stack in ''if ... then''');
      CopyStack(tstack, tstackLen, blocks[bi].savedStack, blocks[bi].savedLen);
      unreachable := blocks[bi].savedUnreach;
      Patch32(blocks[bi].patchElse, EncB(8, RV_ZERO, RV_T0, 1));
      Patch32(blocks[bi].patchElse + 4,
              EncJ(LongInt(CodePC) - LongInt(blocks[bi].patchElse + 4), RV_ZERO));
    end else begin
      if (not blocks[bi].ifUnreach) and (not unreachable) then begin
        if not StackEq(tstack, tstackLen, blocks[bi].elseStack, blocks[bi].elseLen) then
          Die('branch stack mismatch between ''if'' and ''else''');
      end else if (not blocks[bi].ifUnreach) and unreachable then begin
        CopyStack(tstack, tstackLen, blocks[bi].elseStack, blocks[bi].elseLen);
        unreachable := False;
      end;
      Patch32(blocks[bi].patchEnd, EncJ(LongInt(CodePC) - LongInt(blocks[bi].patchEnd), RV_ZERO));
    end;
    Dec(blockCount);
  end else if tok = 'begin' then begin
    if blockCount >= MAX_BLOCKS then Die('too many nested blocks');
    bi := blockCount;
    Inc(blockCount);
    blocks[bi].kind := bkBegin;
    blocks[bi].loopAddr := CodePC;
    CopyStack(blocks[bi].savedStack, blocks[bi].savedLen, tstack, tstackLen);
  end else if tok = 'while' then begin
    if (blockCount = 0) or (blocks[blockCount - 1].kind <> bkBegin) then
      Die('''while'' without ''begin''');
    bi := blockCount - 1;
    TPopExpect(tyW);
    PopReg(RV_T0);
    blocks[bi].patchEnd := CodePC;
    Emit32(0);
    Emit32(0);
  end else if tok = 'repeat' then begin
    if (blockCount = 0) or (blocks[blockCount - 1].kind <> bkBegin) then
      Die('''repeat'' without ''begin''');
    bi := blockCount - 1;
    if not StackEq(tstack, tstackLen, blocks[bi].savedStack, blocks[bi].savedLen) then
      Die('loop body must leave stack even at ''repeat''');
    Emit32(EncJ(LongInt(blocks[bi].loopAddr) - LongInt(CodePC), RV_ZERO));
    Patch32(blocks[bi].patchEnd, EncB(8, RV_ZERO, RV_T0, 1));
    Patch32(blocks[bi].patchEnd + 4,
            EncJ(LongInt(CodePC) - LongInt(blocks[bi].patchEnd + 4), RV_ZERO));
    Dec(blockCount);
  end else if tok = '::' then begin
    if blockCount <> 0 then Die('''::'' inside control block');
    if not NextTok(lblName) then Die('expected label name after ''::''');
    if (not unreachable) and (tstackLen <> 0) then Die('stack must be empty at ''::''');
    tstackLen := 0;
    unreachable := False;
    lidx := FindOrAddLabel(lblName);
    if labels[lidx].defined then Die('duplicate label');
    labels[lidx].defined := True;
    labels[lidx].addr := CodePC;
  end else if tok = 'jmp' then begin
    if not NextTok(lblName) then Die('expected label name after ''jmp''');
    if (not unreachable) and (tstackLen <> 0) then Die('stack must be empty at ''jmp''');
    lidx := FindOrAddLabel(lblName);
    if not unreachable then begin
      if labels[lidx].defined then
        Emit32(EncJ(LongInt(labels[lidx].addr) - LongInt(CodePC), RV_ZERO))
      else begin
        if labelRefCount >= MAX_LREFS then Die('too many label references');
        labelRefs[labelRefCount].patchOff := CodePC;
        labelRefs[labelRefCount].labelIdx := lidx;
        Inc(labelRefCount);
        Emit32(0);
      end;
      unreachable := True;
    end;
  end else if tok = 'jz' then begin
    if not NextTok(lblName) then Die('expected label name after ''jz''');
    TPopExpect(tyW);
    if (not unreachable) and (tstackLen <> 0) then
      Die('stack must be empty after condition at ''jz''');
    lidx := FindOrAddLabel(lblName);
    if not unreachable then begin
      PopReg(RV_T0);
      Emit32(EncB(8, RV_ZERO, RV_T0, 1));
      if labels[lidx].defined then
        Emit32(EncJ(LongInt(labels[lidx].addr) - LongInt(CodePC), RV_ZERO))
      else begin
        if labelRefCount >= MAX_LREFS then Die('too many label references');
        labelRefs[labelRefCount].patchOff := CodePC;
        labelRefs[labelRefCount].labelIdx := lidx;
        Inc(labelRefCount);
        Emit32(0);
      end;
    end;
  end else if tok = 'jtab' then begin
    TPopExpect(tyW);
    if (not unreachable) and (tstackLen <> 0) then
      Die('stack must be empty after index at ''jtab''');
    cnt := 0;
    while True do begin
      if not NextTok(lblName) then Die('unexpected EOF in ''jtab'' (missing ''end'')');
      if lblName = 'end' then Break;
      if not unreachable then begin
        if cnt >= MAX_JTAB then Die('too many jtab entries');
        jtabList[cnt] := FindOrAddLabel(lblName);
      end;
      Inc(cnt);
    end;
    if cnt = 0 then Die('empty ''jtab''');
    if not unreachable then begin
      PopReg(RV_T0);
      Emit32(EncI(2, RV_T0, 1, RV_T0, $13));
      Emit32(EncU(0, RV_T1, $17));
      Emit32(EncI(16, RV_T1, 0, RV_T1, $13));
      Emit32(EncR($00, RV_T0, RV_T1, 0, RV_T0, $33));
      Emit32(EncI(0, RV_T0, 0, RV_ZERO, $67));
      for k := 0 to cnt - 1 do begin
        lidx := jtabList[k];
        if labels[lidx].defined then
          Emit32(EncJ(LongInt(labels[lidx].addr) - LongInt(CodePC), RV_ZERO))
        else begin
          if labelRefCount >= MAX_LREFS then Die('too many label references');
          labelRefs[labelRefCount].patchOff := CodePC;
          labelRefs[labelRefCount].labelIdx := lidx;
          Inc(labelRefCount);
          Emit32(0);
        end;
      end;
      unreachable := True;
    end;
  end else
    HandleControlFlow := False;
end;

{ Parse raw machine-code bytes (:i8, :i16, :i32, :ar) until 'end'. }
procedure ParseRawCodeBody; forward;

{ Compile a function body in one pass; patches prologue frame size and label branches at 'end'. }
procedure CompileWordBody(paramCnt: Integer);
var
  tok: TShortName;
  unreachable: Boolean;
  numVal: LongInt;
  fVal: Single;
  idx, k, i: Integer;
  icallSig: TFuncSig;
  framePatchPC, frameSize: Cardinal;
  curOff, sz: LongInt;
begin
  framePatchPC := CodePC;
  Emit32(EncU(0, RV_T0, $37));
  RvAddi(RV_T0, RV_T0, 0);
  RvAddi(RV_SP, RV_SP, -8);
  RvSw(RV_RA, 4, RV_SP);
  RvSw(RV_FP, 0, RV_SP);
  RvAddi(RV_FP, RV_SP, 0);
  Emit32(EncR($20, RV_T0, RV_SP, 0, RV_SP, $33));

  curOff := 0;
  for i := 0 to paramCnt - 1 do begin
    if locals[i].size > 0 then sz := LongInt((locals[i].size + 3) and not Cardinal(3))
    else sz := 4;
    curOff := curOff + sz;
    locals[i].fpOff := -curOff;
  end;
  for i := paramCnt - 1 downto 0 do begin
    PopReg(RV_T0);
    if (locals[i].fpOff >= -2048) and (locals[i].fpOff <= 2047) then
      RvSw(RV_T0, locals[i].fpOff, RV_FP)
    else begin
      RvLi(RV_T1, Cardinal(locals[i].fpOff));
      Emit32(EncR($00, RV_T1, RV_FP, 0, RV_T1, $33));
      RvSw(RV_T0, 0, RV_T1);
    end;
  end;

  labelCount := 0;
  labelRefCount := 0;
  blockCount := 0;
  unreachable := False;

  while NextTok(tok) do begin
    if tok = 'end' then begin
      if blockCount <> 0 then Die('unclosed control block at ''end''');
      for i := 0 to labelRefCount - 1 do begin
        k := labelRefs[i].labelIdx;
        if not labels[k].defined then Die('undefined label');
        Patch32(labelRefs[i].patchOff,
                EncJ(LongInt(labels[k].addr) - LongInt(labelRefs[i].patchOff), RV_ZERO));
      end;
      if not unreachable then begin
        CheckReturnStack;
        EmitFnEpilogue;
      end;
      frameSize := Cardinal(curOff);
      Patch32(framePatchPC, EncU((frameSize + $800) and $FFFFF000, RV_T0, $37));
      Patch32(framePatchPC + 4,
              EncI(LongInt(frameSize) - LongInt((frameSize + $800) and $FFFFF000),
                   RV_T0, 0, RV_T0, $13));
      Exit;
    end;

    if tok = 'var' then begin
      i := ParseLocalVar(True);
      if locals[i].size > 0 then sz := LongInt((locals[i].size + 3) and not Cardinal(3))
      else sz := 4;
      curOff := curOff + sz;
      locals[i].fpOff := -curOff;
      if locals[i].size = 0 then begin
        if (locals[i].fpOff >= -2048) and (locals[i].fpOff <= 2047) then
          RvSw(RV_ZERO, locals[i].fpOff, RV_FP)
        else begin
          RvLi(RV_T1, Cardinal(locals[i].fpOff));
          Emit32(EncR($00, RV_T1, RV_FP, 0, RV_T1, $33));
          RvSw(RV_ZERO, 0, RV_T1);
        end;
      end;
      Continue;
    end;

    if HandleControlFlow(tok, unreachable) then Continue;

    if ParseInt(tok, numVal) then begin
      TPush(tyW);
      EmitPushW(numVal);
      Continue;
    end;

    if ParseFloat(tok, fVal) then begin
      TPush(tyF);
      EmitPushW(FloatToBits(fVal));
      Continue;
    end;

    if tok = '->' then begin
      if not NextTok(tok) then Die('expected identifier after ''->''');
      if tok = 'var' then begin
        i := ParseLocalVar(False);
        curOff := curOff + 4;
        locals[i].fpOff := -curOff;
        TPopExpect(locals[i].ty);
        EmitLocalSet(i);
        Continue;
      end;
      idx := FindLocal(tok);
      if idx >= 0 then begin
        if locals[idx].size > 0 then Die('cannot assign to '':bytes'' local');
        TPopExpect(locals[idx].ty);
        EmitLocalSet(idx);
        Continue;
      end;
      idx := FindBuffer(tok);
      if idx < 0 then Die('unknown variable in ''->''');
      if buffers[idx].ty = tyNone then Die('cannot assign to '':bytes'' or ''data'' symbol');
      TPopExpect(buffers[idx].ty);
      EmitPushData(buffers[idx].offset);
      EmitGlobalStore;
      Continue;
    end;

    if HandleVarOp(tok) then Continue;

    if tok = '(' then begin
      FillChar(icallSig, SizeOf(TFuncSig), 0);
      ParseSigBody(icallSig, False);
      TPopExpect(tyA);
      for k := icallSig.inCount - 1 downto 0 do
        TPopExpect(icallSig.inTypes[k]);
      for k := 0 to icallSig.outCount - 1 do
        TPush(icallSig.outTypes[k]);
      PopReg(RV_T0);
      Emit32(EncI(0, RV_T0, 0, RV_RA, $67));
      Continue;
    end;

    if HandleBuiltin(tok) then Continue;

    if tok = 'inline' then begin
      ParseRawCodeBody;
      Continue;
    end;

    if tok = '__heap_start' then begin
      EmitPushHeapStart;
      TPush(tyA);
      Continue;
    end;

    idx := FindLocal(tok);
    if idx >= 0 then begin
      if locals[idx].size > 0 then begin
        EmitLocalAddr(idx);
        TPush(tyA);
      end else begin
        EmitLocalGet(idx);
        TPush(locals[idx].ty);
      end;
      Continue;
    end;

    idx := FindBuffer(tok);
    if idx >= 0 then begin
      EmitGlobalRead(idx);
      Continue;
    end;

    idx := FindConst(tok);
    if idx >= 0 then begin
      TPush(consts[idx].ty);
      if consts[idx].ty = tyF then EmitPushW(FloatToBits(consts[idx].fVal))
      else EmitPushW(consts[idx].wVal);
      Continue;
    end;

    idx := FindFunc(tok);
    if idx >= 0 then begin
      for k := funcs[idx].inCount - 1 downto 0 do
        TPopExpect(funcs[idx].inTypes[k]);
      for k := 0 to funcs[idx].outCount - 1 do
        TPush(funcs[idx].outTypes[k]);
      if funcs[idx].isExtern then begin
        if (funcs[idx].inCount > 8) or (funcs[idx].outCount > 1) then
          Die('RV32 extern supports at most 8 args and 1 return');
        for k := funcs[idx].inCount - 1 downto 0 do
          PopReg(RV_A0 + Cardinal(k));
        if funcs[idx].hasAddr then begin
          RvLi(RV_T0, funcs[idx].addr);
          Emit32(EncI(0, RV_T0, 0, RV_RA, $67));
        end else
          Die('unresolved extern ''' + funcs[idx].name + ''' on virt');
        if funcs[idx].outCount = 1 then PushReg(RV_A0);
      end else begin
        if funcs[idx].hasAddr then
          Emit32(EncJ(LongInt(funcs[idx].addr) - LongInt(CodePC), RV_RA))
        else begin
          if fwdCallCount >= MAX_FWDCALLS then Die('too many forward calls');
          fwdCalls[fwdCallCount].patchOff := CodePC;
          fwdCalls[fwdCallCount].funcIdx := idx;
          fwdCalls[fwdCallCount].isFnPtr := False;
          Inc(fwdCallCount);
          Emit32(0);
        end;
      end;
      Continue;
    end;

    Die('unknown word ''' + tok + '''');
  end;
  Die('unexpected EOF inside word definition');
end;

{ --- Top-Level Directives --- }

{ Parse raw machine-code bytes (:i8, :i16, :i32, :ar) until 'end'. }
procedure ParseRawCodeBody;
var
  tok: TShortName;
  numVal, pending: LongInt;
  hasPending: Boolean;
  basePC: Cardinal;
  v16: Word;
  v32: Cardinal;
begin
  basePC := CodePC;
  hasPending := False;
  pending := 0;
  while NextTok(tok) do begin
    if tok = 'end' then begin
      if hasPending then Die('missing width (:i8, :i16, :i32) in raw code');
      Exit;
    end else if tok = ':i8' then begin
      if not hasPending then Die('expected integer before '':i8''');
      if (pending < -128) or (pending > 255) then Die(':i8 value out of range');
      Emit8(Byte(pending and $FF));
      hasPending := False;
    end else if tok = ':i16' then begin
      if not hasPending then Die('expected integer before '':i16''');
      if (pending < -32768) or (pending > 65535) then Die(':i16 value out of range');
      v16 := Word(pending and $FFFF);
      Emit8(Byte(v16 and $FF));
      Emit8(Byte((v16 shr 8) and $FF));
      hasPending := False;
    end else if tok = ':i32' then begin
      if not hasPending then Die('expected integer before '':i32''');
      v32 := Cardinal(pending);
      Emit32(v32);
      hasPending := False;
    end else if tok = ':ar' then begin
      if not hasPending then Die('expected offset before '':ar''');
      if (pending < 0) or (pending > $FFFF) then Die('relocation offset out of range');
      v16 := Word((basePC + Cardinal(pending)) and $FFFF);
      Emit8(Byte(v16 and $FF));
      Emit8(Byte((v16 shr 8) and $FF));
      hasPending := False;
    end else if tok = ':as' then begin
      Die(''':as'' is only supported on targets with a RAM data stack');
    end else if ParseInt(tok, numVal) then begin
      if hasPending then Die('missing width (:i8, :i16, :i32) before literal in raw code');
      pending := numVal;
      hasPending := True;
    end else
      Die('unexpected token in raw code');
  end;
  Die('unexpected EOF in raw code (missing ''end'')');
end;

{ Compile a 'code ... end' machine-code word and append RV32 RET (JALR x0, 0(ra)). }
procedure CompileCode;
var
  name, tok: TShortName;
  sig: TFuncSig;
  fidx, existing: Integer;
  curGrp, curRnk: Integer;
begin
  curGrp := srcStack[srcDepth - 1].reqGroup;
  curRnk := srcStack[srcDepth - 1].reqRank;
  if not NextTok(name) then Die('expected word name after ''code''');
  existing := FindFunc(name);
  if existing >= 0 then begin
    if not funcs[existing].isDefined then begin
      if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after code name');
      FillChar(sig, SizeOf(TFuncSig), 0);
      sig.name := name;
      sig.isDefined := True;
      sig.reqGroup := curGrp;
      sig.reqRank := curRnk;
      ParseSigBody(sig, False);
      if not SigsMatch(sig, funcs[existing]) then
        Die('''code'' signature does not match ''fwd'' declaration');
      fidx := existing;
      funcs[fidx] := sig;
    end else if (curGrp <> 0) and (funcs[existing].reqGroup = curGrp) and
                (curRnk <> funcs[existing].reqRank) then begin
      if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after code name');
      FillChar(sig, SizeOf(TFuncSig), 0);
      sig.name := name;
      sig.isDefined := True;
      sig.reqGroup := curGrp;
      sig.reqRank := curRnk;
      ParseSigBody(sig, False);
      if not SigsMatch(sig, funcs[existing]) then Die('override signature does not match');
      if curRnk > funcs[existing].reqRank then begin
        ParseRawCodeBody;
        Exit;
      end;
      fidx := existing;
      funcs[fidx] := sig;
    end else
      Die('duplicate function definition ''' + name + '''');
  end else begin
    if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after code name');
    fidx := AllocFunc;
    funcs[fidx].name := name;
    funcs[fidx].isDefined := True;
    funcs[fidx].reqGroup := curGrp;
    funcs[fidx].reqRank := curRnk;
    ParseSigBody(funcs[fidx], False);
  end;
  funcs[fidx].addr := CodePC;
  funcs[fidx].hasAddr := True;
  ParseRawCodeBody;
  Emit32($00008067); { RET (JALR x0, 0(ra)) }
end;

{ Compile a 'fn ... end' definition, resolving 'fwd', 'default', and require-rank overrides. }
procedure CompileWord(isDefault: Boolean);
var
  name, tok: TShortName;
  sig: TFuncSig;
  fidx, existing, i: Integer;
  curGrp, curRnk: Integer;
  discardBody: Boolean;
  savedCodeLen: Cardinal;
begin
  curGrp := srcStack[srcDepth - 1].reqGroup;
  curRnk := srcStack[srcDepth - 1].reqRank;
  if not NextTok(name) then Die('expected word name after ''fn''');
  if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after word name');

  localCount := 0;
  FillChar(sig, SizeOf(TFuncSig), 0);
  sig.name := name;
  sig.isDefined := True;
  sig.isDefault := isDefault;
  sig.reqGroup := curGrp;
  sig.reqRank := curRnk;
  ParseSigBody(sig, True);

  discardBody := False;
  existing := FindFunc(name);
  if existing >= 0 then begin
    if not funcs[existing].isDefined then begin
      if not SigsMatch(sig, funcs[existing]) then
        Die('definition signature does not match ''fwd'' declaration');
      fidx := existing;
      funcs[fidx] := sig;
    end else if funcs[existing].isDefault and (not isDefault) then begin
      if not SigsMatch(sig, funcs[existing]) then
        Die('override signature does not match ''default'' definition');
      fidx := existing;
      funcs[fidx] := sig;
    end else if (not funcs[existing].isDefault) and isDefault then begin
      if not SigsMatch(sig, funcs[existing]) then
        Die('''default'' signature does not match existing definition');
      discardBody := True;
      fidx := existing;
    end else if (curGrp <> 0) and (funcs[existing].reqGroup = curGrp) and
              (curRnk <> funcs[existing].reqRank) then begin
      if not SigsMatch(sig, funcs[existing]) then
        Die('override signature does not match');
      if curRnk > funcs[existing].reqRank then begin
        discardBody := True;
        fidx := existing;
      end else begin
        fidx := existing;
        funcs[fidx] := sig;
      end;
    end else
      Die('duplicate function definition ''' + name + '''');
  end else begin
    fidx := AllocFunc;
    funcs[fidx] := sig;
  end;

  if not discardBody then begin
    funcs[fidx].addr := CodePC;
    funcs[fidx].hasAddr := True;
  end;
  tstackLen := 0;
  if localCount = 0 then
    for i := 0 to sig.inCount - 1 do TPush(sig.inTypes[i]);
  curOutCount := sig.outCount;
  for i := 0 to sig.outCount - 1 do curOuts[i] := sig.outTypes[i];

  savedCodeLen := codeLen;
  CompileWordBody(localCount);
  if discardBody then codeLen := savedCodeLen;
end;

procedure CompileInit;
var
  fidx: Integer;
begin
  localCount := 0;
  tstackLen := 0;
  curOutCount := 0;
  fidx := AllocFunc;
  funcs[fidx].name := '__init_' + IntToStrSimple(initCount);
  funcs[fidx].isDefined := True;
  funcs[fidx].addr := CodePC;
  funcs[fidx].hasAddr := True;
  if initCount >= MAX_INITS then Die('too many init blocks');
  initFuncs[initCount] := fidx;
  Inc(initCount);
  CompileWordBody(0);
end;

procedure CompileConst(isDefault: Boolean);
var
  name, valTok: TShortName;
  cs: TConstSym;
  iv: LongInt;
  fv: Single;
  exIdx: Integer;
begin
  if not NextTok(name) then Die('expected constant name after ''const''');
  if (FindBuffer(name) >= 0) or (FindFunc(name) >= 0) then
    Die('constant name conflicts with existing global or function');
  if not NextTok(valTok) then Die('expected value after constant name');

  cs.name := name;
  cs.isDefault := isDefault;
  cs.reqGroup := srcStack[srcDepth - 1].reqGroup;
  cs.reqRank := srcStack[srcDepth - 1].reqRank;
  if ParseInt(valTok, iv) then begin
    cs.ty := tyW;
    cs.wVal := iv;
    cs.fVal := 0;
  end else if ParseFloat(valTok, fv) then begin
    cs.ty := tyF;
    cs.wVal := 0;
    cs.fVal := fv;
  end else
    Die('invalid constant value');

  exIdx := FindConst(name);
  if exIdx >= 0 then begin
    if consts[exIdx].isDefault and (not isDefault) then consts[exIdx] := cs
    else if (not consts[exIdx].isDefault) and isDefault then Exit
    else if (cs.reqGroup <> 0) and (consts[exIdx].reqGroup = cs.reqGroup) and
            (cs.reqRank <> consts[exIdx].reqRank) then begin
      if cs.reqRank < consts[exIdx].reqRank then consts[exIdx] := cs;
    end else
      Die('duplicate constant ''' + name + '''');
  end else begin
    if constCount >= MAX_CONSTS then Die('too many constants');
    consts[constCount] := cs;
    Inc(constCount);
  end;
end;

{ Compile a 'data ... end' block with strings, typed literals, 'align', and '&' relocations. }
procedure CompileData;
var
  name, tok, s: TShortName;
  ch, curIdx, idx: Integer;
  numVal, pending: LongInt;
  hasPending: Boolean;
begin
  if not NextTok(name) then Die('expected name after ''data''');
  if FindBuffer(name) >= 0 then Die('duplicate global/data name');
  if bufferCount >= MAX_BUFFERS then Die('too many globals/data');
  buffers[bufferCount].name := name;
  buffers[bufferCount].offset := AllocData(0, 4);
  buffers[bufferCount].ty := tyNone;
  Inc(bufferCount);

  hasPending := False;
  pending := 0;
  while True do begin
    ch := SkipWsAndComments;
    if ch < 0 then Break;
    if ch = Ord('"') then begin
      if hasPending then begin
        EmitDataU32(Cardinal(pending));
        hasPending := False;
      end;
      curIdx := srcDepth - 1;
      while True do begin
        ch := SrcGetC(srcStack[curIdx]);
        if (ch < 0) or (ch = 10) then Die('unterminated string literal in ''data''');
        if ch = Ord('"') then Break;
        if ch = Ord('\') then begin
          ch := SrcGetC(srcStack[curIdx]);
          case Chr(ch) of
            'n': ch := 10;
            'r': ch := 13;
            't': ch := 9;
            '0': ch := 0;
            '\': ch := Ord('\');
            '"': ch := Ord('"');
          else
            Die('unknown escape sequence in string literal');
          end;
        end;
        EmitDataU8(Byte(ch));
      end;
      Continue;
    end;
    SrcUngetC(srcStack[srcDepth - 1], ch);
    if not NextTok(tok) then Break;
    if tok = 'end' then begin
      if hasPending then EmitDataU32(Cardinal(pending));
      Exit;
    end;
    if tok = 'align' then begin
      if hasPending then begin
        EmitDataU32(Cardinal(pending));
        hasPending := False;
      end;
      AllocData(0, 4);
      dataInitSize := dataLen;
      Continue;
    end;
    if tok = '&' then begin
      if hasPending then begin
        EmitDataU32(Cardinal(pending));
        hasPending := False;
      end;
      if not NextTok(s) then Die('expected symbol after ''&'' in ''data''');
      idx := FindFunc(s);
      if idx >= 0 then begin
        if funcs[idx].isExtern then Die('cannot take address of ''extern'' in ''data''');
        if dataRelocCount >= MAX_DRELOCS then Die('too many data relocs');
        dataRelocs[dataRelocCount].dataOff := dataLen;
        dataRelocs[dataRelocCount].targetVal := Cardinal(idx);
        dataRelocs[dataRelocCount].isCode := True;
        Inc(dataRelocCount);
        EmitDataU32(0);
      end else begin
        idx := FindBuffer(s);
        if idx < 0 then Die('unknown symbol in ''data'' ''&''');
        if dataRelocCount >= MAX_DRELOCS then Die('too many data relocs');
        dataRelocs[dataRelocCount].dataOff := dataLen;
        dataRelocs[dataRelocCount].targetVal := buffers[idx].offset;
        dataRelocs[dataRelocCount].isCode := False;
        Inc(dataRelocCount);
        EmitDataU32(0);
      end;
      Continue;
    end;
    if (tok = ':i8') or (tok = ':i16') or (tok = ':i32') or (tok = ':i') then begin
      if not hasPending then Die('expected value before ''' + tok + '''');
      if tok = ':i8' then EmitDataU8(Byte(pending and $FF))
      else if tok = ':i16' then EmitDataU16(Word(pending and $FFFF))
      else EmitDataU32(Cardinal(pending));
      hasPending := False;
      Continue;
    end;
    if ParseInt(tok, numVal) then begin
      if hasPending then EmitDataU32(Cardinal(pending));
      pending := numVal;
      hasPending := True;
      Continue;
    end;
    Die('unexpected token ''' + tok + ''' in ''data''');
  end;
  Die('unexpected EOF inside ''data'' (missing ''end'')');
end;

procedure ParseTopLevel;
var
  tok, name: TShortName;
  isDefault: Boolean;
  sig: TFuncSig;
  fidx, existing: Integer;
  numVal: LongInt;
  ty: TStackType;
begin
  while NextTok(tok) do begin
    if tok = 'fn' then
      CompileWord(False)
    else if tok = 'const' then
      CompileConst(False)
    else if (tok = 'default') or (tok = 'hot') then begin
      isDefault := False;
      while (tok = 'default') or (tok = 'hot') do begin
        if tok = 'default' then isDefault := True;
        if not NextTok(tok) then Break;
      end;
      if tok = 'code' then begin
        if isDefault then Die('''default'' is not supported on ''code''');
        CompileCode;
      end else if tok = 'const' then CompileConst(isDefault)
      else if tok = 'fn' then CompileWord(isDefault)
      else Die('expected ''fn'', ''const'', or ''code'' after modifier');
    end else if tok = 'code' then
      CompileCode
    else if tok = 'init' then
      CompileInit
    else if tok = 'fwd' then begin
      if not NextTok(name) then Die('expected word name after ''fwd''');
      if FindFunc(name) >= 0 then Die('duplicate ''fwd'' declaration');
      if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after ''fwd'' name');
      fidx := AllocFunc;
      funcs[fidx].name := name;
      ParseSigBody(funcs[fidx], False);
    end else if tok = 'extern' then begin
      if not NextTok(name) then Die('expected word name after ''extern''');
      FillChar(sig, SizeOf(TFuncSig), 0);
      sig.name := name;
      sig.isExtern := True;
      sig.isDefined := True;
      sig.reqGroup := srcStack[srcDepth - 1].reqGroup;
      sig.reqRank := srcStack[srcDepth - 1].reqRank;
      if not NextTok(tok) then Die('expected address or ''('' after extern name');
      if tok <> '(' then begin
        if not ParseInt(tok, numVal) then Die('expected address or ''('' after extern name');
        sig.addr := Cardinal(numVal);
        sig.hasAddr := True;
        if not NextTok(tok) or (tok <> '(') then Die('expected ''('' after extern address');
      end;
      ParseSigBody(sig, False);
      existing := FindFunc(name);
      if existing >= 0 then begin
        if not funcs[existing].isDefined then begin
          if not SigsMatch(sig, funcs[existing]) then Die('extern signature does not match ''fwd''');
          funcs[existing] := sig;
        end else if (sig.reqGroup <> 0) and (funcs[existing].reqGroup = sig.reqGroup) and
                  (sig.reqRank <> funcs[existing].reqRank) then begin
          if not SigsMatch(sig, funcs[existing]) then Die('override extern signature does not match');
          if sig.reqRank < funcs[existing].reqRank then funcs[existing] := sig;
        end else
          Die('duplicate function definition');
      end else begin
        fidx := AllocFunc;
        funcs[fidx] := sig;
      end;
    end else if tok = 'require' then
      PushRequire(ReadRequirePath)
    else if tok = 'data' then
      CompileData
    else if tok = 'var' then begin
      if not NextTok(name) then Die('expected var name after ''var''');
      if FindBuffer(name) >= 0 then Die('duplicate global/data name');
      if bufferCount >= MAX_BUFFERS then Die('too many globals/data');
      buffers[bufferCount].name := name;
      if not NextTok(tok) then Die('expected type after var name');
      if tok = ':bytes' then begin
        if not NextTok(tok) or not ParseInt(tok, numVal) or (numVal <= 0) then
          Die('invalid byte count after '':bytes''');
        buffers[bufferCount].offset := AllocData(Cardinal(numVal), 4);
        buffers[bufferCount].ty := tyNone;
      end else begin
        ty := ParseTypeTok(tok);
        if ty = tyNone then Die('expected '':i'', '':a'', '':f'', '':i32'', or '':bytes''');
        buffers[bufferCount].offset := AllocData(4, 4);
        buffers[bufferCount].ty := ty;
      end;
      Inc(bufferCount);
    end else
      Die('unexpected top-level token ''' + tok + '''');
  end;
end;

{ --- ELF32 virt Stub & Output --- }

{ Patch forward function calls, '& fn' AUIPC+ADDI pairs, and __heap_start references. }
procedure PatchForwardRefs(heapOff: Cardinal);
var
  i: Integer;
  pc, target, hi: Cardinal;
  lo: LongInt;
begin
  for i := 0 to fwdCallCount - 1 do begin
    pc := fwdCalls[i].patchOff;
    if not funcs[fwdCalls[i].funcIdx].hasAddr then
      Die('called ''fwd'' function ''' + funcs[fwdCalls[i].funcIdx].name + ''' was never defined');
    target := funcs[fwdCalls[i].funcIdx].addr;
    if fwdCalls[i].isFnPtr then begin
      Patch32(pc, EncU(0, RV_T0, $17));
      Patch32(pc + 4, EncI(LongInt(target) - LongInt(pc), RV_T0, 0, RV_T0, $13));
    end else
      Patch32(pc, EncJ(LongInt(target) - LongInt(pc), RV_RA));
  end;
  hi := (heapOff + $800) and $FFFFF000;
  lo := LongInt(heapOff - hi);
  for i := 0 to heapRelocCount - 1 do begin
    pc := heapRelocs[i];
    Patch32(pc, EncU(hi, RV_T0, $37));
    Patch32(pc + 4, EncI(lo, RV_T0, 0, RV_T0, $13));
  end;
end;

{ Emit the QEMU virt startup stub (enable FPU, init sp/s1/s2, run inits+main, print/exit) and ELF32 file. }
procedure WriteVirtELF(const outPath: TPathStr);
var
  mainIdx, i: Integer;
  entryOff, dbPatchOff, divLoop, printLoop, dataAddr, hi, filesz, memsz, val: Cardinal;
  lo: LongInt;
  hdr: array[0..83] of Byte;
  f: file;
begin
  mainIdx := FindFunc('main');
  if mainIdx < 0 then Die('no ''main'' function defined');
  if (funcs[mainIdx].inCount <> 0) or (funcs[mainIdx].outCount > 1) then
    Die('''main'' must have 0 inputs and at most 1 output');

  entryOff := CodePC;
  Patch32(0, EncJ(LongInt(entryOff), RV_ZERO));

  Emit32(EncU($80200000, RV_SP, $37));
  Emit32(EncU($80100000, RV_DSP, $37));
  Emit32(EncU($2000, RV_T0, $37));              { LUI t0, 0x2000 }
  Emit32(EncI($300, RV_T0, 2, RV_ZERO, $73));   { CSRRS x0, mstatus, t0 (enable FPU) }
  dbPatchOff := CodePC;
  Emit32(EncU(0, RV_DB, $37));
  RvAddi(RV_DB, RV_DB, 0);

  for i := 0 to initCount - 1 do
    Emit32(EncJ(LongInt(funcs[initFuncs[i]].addr) - LongInt(CodePC), RV_RA));

  Emit32(EncJ(LongInt(funcs[mainIdx].addr) - LongInt(CodePC), RV_RA));

  if funcs[mainIdx].outCount = 1 then begin
    Emit32(EncI(0, RV_DSP, 2, RV_A0, $03));
    Emit32(EncI(4, RV_DSP, 0, RV_DSP, $13));
    Emit32(EncU(VIRT_UART0, 7, $37));
    Emit32(EncB(16, 0, RV_A0, 5));
    Emit32(EncI(Ord('-'), 0, 0, 5, $13));
    Emit32(EncS(0, 5, 7, 0));
    Emit32(EncR($20, RV_A0, 0, 0, RV_A0, $33));
    Emit32(EncI(0, RV_SP, 0, 28, $13));
    Emit32(EncI(10, 0, 0, 6, $13));
    divLoop := CodePC;
    Emit32(EncR($01, 6, RV_A0, 7, 5, $33));
    Emit32(EncR($01, 6, RV_A0, 5, RV_A0, $33));
    Emit32(EncI(Ord('0'), 5, 0, 5, $13));
    Emit32(EncI(-4, RV_SP, 0, RV_SP, $13));
    Emit32(EncS(0, 5, RV_SP, 2));
    Emit32(EncB(LongInt(divLoop) - LongInt(CodePC), 0, RV_A0, 1));
    printLoop := CodePC;
    Emit32(EncI(0, RV_SP, 2, 5, $03));
    Emit32(EncI(4, RV_SP, 0, RV_SP, $13));
    Emit32(EncS(0, 5, 7, 0));
    Emit32(EncB(LongInt(printLoop) - LongInt(CodePC), 28, RV_SP, 1));
    Emit32(EncI(10, 0, 0, 5, $13));
    Emit32(EncS(0, 5, 7, 0));
  end;

  Emit32(EncU(VIRT_TEST, 5, $37));
  Emit32(EncU($00005000, 6, $37));
  Emit32(EncI($555, 6, 0, 6, $13));
  Emit32(EncS(0, 6, 5, 2));
  Emit32(EncJ(0, 0));

  dataAddr := VIRT_BASE + CodePC;
  for i := 0 to dataRelocCount - 1 do begin
    if dataRelocs[i].isCode then
      val := VIRT_BASE + funcs[dataRelocs[i].targetVal].addr
    else
      val := dataAddr + dataRelocs[i].targetVal;
    PutLE32(dataBuf, Integer(dataRelocs[i].dataOff), val);
  end;
  hi := (dataAddr + $800) and $FFFFF000;
  lo := LongInt(dataAddr - hi);
  Patch32(dbPatchOff, EncU(hi, RV_DB, $37));
  Patch32(dbPatchOff + 4, EncI(lo, RV_DB, 0, RV_DB, $13));

  FillChar(hdr, SizeOf(hdr), 0);
  hdr[0] := $7F; hdr[1] := Ord('E'); hdr[2] := Ord('L'); hdr[3] := Ord('F');
  hdr[4] := 1; hdr[5] := 1; hdr[6] := 1;
  PutLE16(hdr, 16, 2);
  PutLE16(hdr, 18, 243);
  PutLE32(hdr, 20, 1);
  PutLE32(hdr, 24, VIRT_BASE);
  PutLE32(hdr, 28, 52);
  PutLE16(hdr, 40, 52);
  PutLE16(hdr, 42, 32);
  PutLE16(hdr, 44, 1);

  filesz := CodePC + dataInitSize;
  memsz := CodePC + dataLen;
  PutLE32(hdr, 52, 1);
  PutLE32(hdr, 56, 84);
  PutLE32(hdr, 60, VIRT_BASE);
  PutLE32(hdr, 64, VIRT_BASE);
  PutLE32(hdr, 68, filesz);
  PutLE32(hdr, 72, memsz);
  PutLE32(hdr, 76, 7);
  PutLE32(hdr, 80, 4);

  Assign(f, outPath);
  Rewrite(f, 1);
  BlockWrite(f, hdr, SizeOf(hdr));
  if CodePC > 0 then BlockWrite(f, codeBuf, CodePC);
  if dataInitSize > 0 then BlockWrite(f, dataBuf, dataInitSize);
  Close(f);
end;

procedure InitBuiltinConsts;
const
  KNOWN_TARGETS: array[0..14] of TShortName = (
    'z80', 'ez80', '6502', 'arm64', 'rv32', 'bc',
    'cpm', 'zx81', 'agon', 'sim65', 'c64', 'x16', 'virt', 'obj', 'jit'
  );
var
  i: Integer;
begin
  AddBuiltinConst('true', -1);
  AddBuiltinConst('false', 0);
  AddBuiltinConst('size_i', 4);
  AddBuiltinConst('size_a', 4);
  AddBuiltinConst('size_f', 4);
  for i := 0 to 14 do begin
    if (KNOWN_TARGETS[i] = 'rv32') or (KNOWN_TARGETS[i] = 'virt') then
      AddBuiltinConst('target_' + KNOWN_TARGETS[i], -1)
    else
      AddBuiltinConst('target_' + KNOWN_TARGETS[i], 0);
  end;
end;

{ --- Main Driver --- }

var
  inPath, outPath, arg: TPathStr;
  i: Integer;
  heapOff: Cardinal;
begin
  srcDepth := 0;
  seenCount := 0;
  extraRootCount := 0;
  nextReqGroup := 1;
  codeLen := 0;
  dataLen := 0;
  dataInitSize := 0;
  funcCount := 0;
  bufferCount := 0;
  constCount := 0;
  initCount := 0;
  fwdCallCount := 0;
  heapRelocCount := 0;
  dataRelocCount := 0;

  outPath := 'a.out';
  inPath := '';
  i := 1;
  while i <= ParamCount do begin
    arg := ParamStr(i);
    if (arg = '-o') and (i + 1 <= ParamCount) then begin
      Inc(i);
      outPath := ParamStr(i);
    end else if (arg = '-I') and (i + 1 <= ParamCount) then begin
      Inc(i);
      if extraRootCount >= MAX_ROOTS then Die('too many -I roots');
      extraLibRoots[extraRootCount] := ParamStr(i);
      Inc(extraRootCount);
    end else if (Length(arg) > 0) and (arg[1] <> '-') then
      inPath := arg;
    Inc(i);
  end;
  if inPath = '' then begin
    Writeln(StdErr, 'Usage: ir4p [-I dir]... [-o out.elf] <input.ir4>');
    Halt(1);
  end;

  InitBuiltinConsts;
  if not OpenSrcFile(inPath, 0, 0) then
    Die('cannot open input file ''' + inPath + '''');
  Emit32(0);
  ParseTopLevel;
  heapOff := AllocData(0, 4);
  PatchForwardRefs(heapOff);
  WriteVirtELF(outPath);
end.

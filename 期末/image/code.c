#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <dlfcn.h>

//此處引入標頭檔 C4會自動忽略並轉為類似printf等類函式庫


char *p, *lp, // 當前源代碼位置 (*:指標 p: 目前原始碼指標, lp: 上一行原始碼指標)
     *jitmem, // IT 編譯的本地代碼的可執行內存
     *data,   // 資料段機器碼指標
     **linemap; // 將行號映射到其源位置

int *e, *le, *text, // 發出代碼目前位置 (e: 目前機器碼指標, le: 上一行機器碼指標)
    *id,      // 目前解析出來的標識名稱(符) (id: 目前的 id)
    *sym,     // 簡單的標識列表 (符號表)
    tk,       // 當前標記(目前 token)
    ival,     // 當前標記值 (目前的 token 值)
    ty,       // 當前表示式型態 (目前的運算式型態)
    loc,      // 區域變數位移 (區域變數的位移)
    line,     // 目前行號 (目前行號)
    *srcmap,  // 將字節碼映射到對應的源行號
    src;      // 印出原始碼和程序標誌 (印出原始碼)

// tokens and classes (運算符最後並按優先順序排列) (按優先權順序排列)
enum Token {// token(標記) : 0-127 直接用該字母表達， 128 以後用代號。
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

enum Opcode {
  LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,
  OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,
  OPEN,READ,CLOS,PRTF,MALC,MSET,MCMP,MCPY,MMAP,DOPN,DSYM,QSRT,EXIT
};

enum Ty { CHAR, INT, PTR };

// identifier offsets (since we can't create an ident struct)
enum Identifier { Tk, Hash, Name, Class, Type, Val, HClass, HType, HVal, Idsz };

void next() // 詞彙解析 lexer
{
  char *pp; // 用循環來忽略空白字符,不過不能被詞法分析器識別的字符都被認為是空白字符

  while (tk = *p) {
    ++p;
    if (tk == '\n') { // 換行
      if (src) {
        linemap[line] = lp;
        while (le < e) { srcmap[le - text] = line; le++; }; //  當上一行機器碼指標小於目前機器碼指標時 印出上一行的所有目的碼
      }
      lp = p; // lp = p = 新一行的原始碼開頭
      ++line;
    }
    else if (tk == '#') { // 取得 #include <stdio.h> 引入標頭檔這類的一整行
      while (*p != 0 && *p != '\n') ++p; //當目前原始碼指標不等於零and不為空值
    }
    else if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || tk == '_') { // 取得變數名稱
      pp = p - 1; //因为有++p,pp回退一个字符,pp指向 [这个符號] 的首字母
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') //目前原始碼指標(大於等於a and 小於等於z)or(大於等於A and 小於等於Z)or(大於等於0 and 小於等於9)or為空值
        tk = tk * 147 + *p++; // 計算雜湊值
      tk = (tk << 6) + (p - pp); // 符號表的雜湊位址
      id = sym; //  目前解析出來的名稱=列表
      while (id[Tk]) { // 檢查是否碰撞
        if (tk == id[Hash] && !memcmp((char *)id[Name], pp, p - pp)) { tk = id[Tk]; return; } // 沒碰撞就傳回
        id = id + Idsz; // 碰撞，前進到下一格。
      }
      id[Name] = (int)pp; // id.Name = ptr(變數名稱)
      id[Hash] = tk; // id.Hash = 雜湊值
      tk = id[Tk] = Id; // token = id.Tk = Id  token 類型為 identifier
      return;
    }
    else if (tk >= '0' && tk <= '9') { // 取得數字串
      if (ival = tk - '0') { while (*p >= '0' && *p <= '9') ival = ival * 10 + *p++ - '0'; } // 十進位
      else if (*p == 'x' || *p == 'X') { // 十六進位  「x」則代表十六進位（就如「O」代表八進位)
        while ((tk = *++p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F'))) // 16 進位
          ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
      }
      else { while (*p >= '0' && *p <= '7') ival = ival * 8 + *p++ - '0'; } // 八進位
      tk = Num;
      return;
    }
    else if (tk == '/') {
      if (*p == '/') {  // 註解
        ++p;
        while (*p != 0 && *p != '\n') ++p; // 略過註解
      }
      else {
        tk = Div;
        return;
      }
    }
    else if (tk == '\'' || tk == '"') {
      pp = data;
      while (*p != 0 && *p != tk) {
        if ((ival = *p++) == '\\') {
          if ((ival = *p++) == 'n') ival = '\n';
        }
        if (tk == '"') *data++ = ival; // 把字串塞到資料段裏(如果是"則視為字符串，向資料段輸入字符)
      }
      ++p;
      if (tk == '"') ival = (int)pp; else tk = Num; // (若是字串) ? (ival = 字串 (在資料段中的) 指標) : (字元值) 
      //雙引號(")則ival指向data中字符串开始,單引號則視為数字
      return;
    } // 以下為運算元 =+-!<>|&^%*[?~, ++, --, !=, <=, >=, ||, &&, ~  ;{}()],:
    else if (tk == '=') { if (*p == '=') { ++p; tk = Eq; } else tk = Assign; return; } //等於,賦值
    else if (tk == '+') { if (*p == '+') { ++p; tk = Inc; } else tk = Add; return; } //加
    else if (tk == '-') { if (*p == '-') { ++p; tk = Dec; } else tk = Sub; return; } //減
    else if (tk == '!') { if (*p == '=') { ++p; tk = Ne; } return; } //不等於
    else if (tk == '<') { if (*p == '=') { ++p; tk = Le; } else if (*p == '<') { ++p; tk = Shl; } else tk = Lt; return; } //< ， <= ，<<
    else if (tk == '>') { if (*p == '=') { ++p; tk = Ge; } else if (*p == '>') { ++p; tk = Shr; } else tk = Gt; return; } //> ， >= ，>>
    else if (tk == '|') { if (*p == '|') { ++p; tk = Lor; } else tk = Or; return; } //或 
    else if (tk == '&') { if (*p == '&') { ++p; tk = Lan; } else tk = And; return; } //和
    else if (tk == '^') { tk = Xor; return; }
    else if (tk == '%') { tk = Mod; return; }
    else if (tk == '*') { tk = Mul; return; }
    else if (tk == '[') { tk = Brak; return; }
    else if (tk == '?') { tk = Cond; return; }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return;
  }
}

void expr(int lev) // 表示式分析(運算式) expression, 其中 lev 代表優先等級
{
  int t, *d;

  if (!tk) { printf("%d: unexpected eof in expression\n", line); exit(-1); } // EOF
  else if (tk == Num) { *++e = IMM; *++e = ival; next(); ty = INT; } // 取數為表達示數值
  else if (tk == '"') { // 字串
    *++e = IMM; *++e = ival; next();
    while (tk == '"') next();
    data = (char *)((int)data + sizeof(int) & -sizeof(int)); ty = PTR; // 用 int 為大小對齊
  }
  else if (tk == Sizeof) { // 處理 sizeof(type) ，其中 type 可能為 char, int 或 ptr
    next(); if (tk == '(') next(); else { printf("%d: open paren expected in sizeof\n", line); exit(-1); }
    ty = INT; if (tk == Int) next(); else if (tk == Char) { next(); ty = CHAR; }
    while (tk == Mul) { next(); ty = ty + PTR; } //多级指標,每多一级加PTR
    if (tk == ')') next(); else { printf("%d: close paren expected in sizeof\n", line); exit(-1); }
    *++e = IMM; *++e = (ty == CHAR) ? sizeof(char) : sizeof(int); //除了char是一字節,int和多级指標都是int大小
    ty = INT;
  }
  else if (tk == Id) { // 處理 id
    d = id; next();
    if (tk == '(') {
      next();
      t = 0; //形式参數
      while (tk != ')') { expr(Assign); *++e = PSH; ++t; if (tk == ',') next(); }
      next();
      if (d[Class] == Sys) *++e = d[Val];
      else if (d[Class] == Fun) { *++e = JSR; *++e = d[Val]; }
      else { printf("%d: bad function call\n", line); exit(-1); }
      if (t) { *++e = ADJ; *++e = t; }
      ty = d[Type];
    }
    else if (d[Class] == Num) { *++e = IMM; *++e = d[Val]; ty = INT; }
    else {
      if (d[Class] == Loc) { *++e = LEA; *++e = loc - d[Val]; }
      else if (d[Class] == Glo) { *++e = IMM; *++e = d[Val]; }
      else { printf("%d: undefined variable\n", line); exit(-1); }
      *++e = ((ty = d[Type]) == CHAR) ? LC : LI;
    }
  }
  else if (tk == '(') {
    next();
    if (tk == Int || tk == Char) {
      t = (tk == Int) ? INT : CHAR; next();
      while (tk == Mul) { next(); t = t + PTR; }
      if (tk == ')') next(); else { printf("%d: bad cast\n", line); exit(-1); }
      expr(Inc);
      ty = t;
    }
    else {
      expr(Assign);
      if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    }
  }
  else if (tk == Mul) {
    next(); expr(Inc);
    if (ty > INT) ty = ty - PTR; else { printf("%d: bad dereference\n", line); exit(-1); }
    *++e = (ty == CHAR) ? LC : LI;
  }
  else if (tk == And) {
    next(); expr(Inc);
    if (*e == LC || *e == LI) --e; else { printf("%d: bad address-of\n", line); exit(-1); }
    ty = ty + PTR;
  }
  else if (tk == '!') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = 0; *++e = EQ; ty = INT; }
  else if (tk == '~') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = -1; *++e = XOR; ty = INT; }
  else if (tk == Add) { next(); expr(Inc); ty = INT; }
  else if (tk == Sub) {
    next(); *++e = IMM;
    if (tk == Num) { *++e = -ival; next(); } else { *++e = -1; *++e = PSH; expr(Inc); *++e = MUL; }
    ty = INT;
  }
  else if (tk == Inc || tk == Dec) {
    t = tk; next(); expr(Inc);
    if (*e == LC) { *e = PSH; *++e = LC; }
    else if (*e == LI) { *e = PSH; *++e = LI; }
    else { printf("%d: bad lvalue in pre-increment\n", line); exit(-1); }
    *++e = PSH;
    *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
    *++e = (t == Inc) ? ADD : SUB;
    *++e = (ty == CHAR) ? SC : SI;
  }
  else { printf("%d: bad expression\n", line); exit(-1); }

  while (tk >= lev) { // "precedence climbing" or "Top Down Operator Precedence" method
    t = ty;
    if (tk == Assign) {
      next();
      if (*e == LC || *e == LI) *e = PSH; else { printf("%d: bad lvalue in assignment\n", line); exit(-1); }
      expr(Assign); *++e = ((ty = t) == CHAR) ? SC : SI;
    }
    else if (tk == Cond) {
      next();
      *++e = BZ; d = ++e;
      expr(Assign);
      if (tk == ':') next(); else { printf("%d: conditional missing colon\n", line); exit(-1); }
      *d = (int)(e + 3); *++e = JMP; d = ++e;
      expr(Cond);
      *d = (int)(e + 1);
    }
    else if (tk == Lor) { next(); *++e = BNZ; d = ++e; expr(Lan); *d = (int)(e + 1); ty = INT; } //短路,邏輯Or運算符左左邊true則表示式為true,不用計算運算符右側的值
    else if (tk == Lan) { next(); *++e = BZ;  d = ++e; expr(Or);  *d = (int)(e + 1); ty = INT; } //短路,邏輯And
    else if (tk == Or)  { next(); *++e = PSH; expr(Xor); *++e = OR;  ty = INT; } //將當前值Push,計算運算符右边值,再與當前值(在棧中)做運算
    else if (tk == Xor) { next(); *++e = PSH; expr(And); *++e = XOR; ty = INT; } //expr中lev指明遞迴函数中最结合性不得低於哪一个運算符
    else if (tk == And) { next(); *++e = PSH; expr(Eq);  *++e = AND; ty = INT; }
    else if (tk == Eq)  { next(); *++e = PSH; expr(Lt);  *++e = EQ;  ty = INT; }
    else if (tk == Ne)  { next(); *++e = PSH; expr(Lt);  *++e = NE;  ty = INT; }
    else if (tk == Lt)  { next(); *++e = PSH; expr(Shl); *++e = LT;  ty = INT; }
    else if (tk == Gt)  { next(); *++e = PSH; expr(Shl); *++e = GT;  ty = INT; }
    else if (tk == Le)  { next(); *++e = PSH; expr(Shl); *++e = LE;  ty = INT; }
    else if (tk == Ge)  { next(); *++e = PSH; expr(Shl); *++e = GE;  ty = INT; }
    else if (tk == Shl) { next(); *++e = PSH; expr(Add); *++e = SHL; ty = INT; }
    else if (tk == Shr) { next(); *++e = PSH; expr(Add); *++e = SHR; ty = INT; }
    else if (tk == Add) {
      next(); *++e = PSH; expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  } //處理指標
      *++e = ADD;
    }
    else if (tk == Sub) {
      next(); *++e = PSH; expr(Mul);
      if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      *++e = SUB;
    }
    else if (tk == Mul) { next(); *++e = PSH; expr(Inc); *++e = MUL; ty = INT; }
    else if (tk == Div) { next(); *++e = PSH; expr(Inc); *++e = DIV; ty = INT; }
    else if (tk == Mod) { next(); *++e = PSH; expr(Inc); *++e = MOD; ty = INT; }
    else if (tk == Inc || tk == Dec) {
      if (*e == LC) { *e = PSH; *++e = LC; }
      else if (*e == LI) { *e = PSH; *++e = LI; }
      else { printf("%d: bad lvalue in post-increment\n", line); exit(-1); }
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? ADD : SUB;
      *++e = (ty == CHAR) ? SC : SI;
      *++e = PSH; *++e = IMM; *++e = (ty > PTR) ? sizeof(int) : sizeof(char);
      *++e = (tk == Inc) ? SUB : ADD;
      next();
    }
    else if (tk == Brak) {
      next(); *++e = PSH; expr(Assign);
      if (tk == ']') next(); else { printf("%d: close bracket expected\n", line); exit(-1); }
      if (t > PTR) { *++e = PSH; *++e = IMM; *++e = sizeof(int); *++e = MUL;  }
      else if (t < PTR) { printf("%d: pointer type expected\n", line); exit(-1); }
      *++e = ADD;
      *++e = ((ty = t - PTR) == CHAR) ? LC : LI;
    }
    else { printf("%d: compiler error tk=%d\n", line, tk); exit(-1); }
  }
}

void stmt()
{
  int *a, *b;

  if (tk == If) {
    next();
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    if (tk == Else) {
      *b = (int)(e + 3); *++e = JMP; b = ++e;
      next();
      stmt();
    }
    *b = (int)(e + 1);
  }
  else if (tk == While) {
    next();
    a = e + 1;
    if (tk == '(') next(); else { printf("%d: open paren expected\n", line); exit(-1); }
    expr(Assign);
    if (tk == ')') next(); else { printf("%d: close paren expected\n", line); exit(-1); }
    *++e = BZ; b = ++e;
    stmt();
    *++e = JMP; *++e = (int)a;
    *b = (int)(e + 1);
  }
  else if (tk == Return) {
    next();
    if (tk != ';') expr(Assign);
    *++e = LEV;
    if (tk == ';') next(); else { printf("%d: semicolon expected\n", line); exit(-1); }
  }
  else if (tk == '{') {
    next();
    while (tk != '}') stmt();
    next();
  }
  else if (tk == ';') {
    next();
  }
  else {
    expr(Assign);
    if (tk == ';') next(); else { printf("%d: semicolon expected\n", line); exit(-1); }
  }
}

int main(int argc, char **argv)
{
  int fd, bt, ty, poolsz, *idmain;
  int *pc;
  int i, tmp; // temps
  void *dl;
  int (*jitmain)();
  char *je,      // current position in emitted native code
       **jitmap;  // maps c4 bytecode index into native code position

  --argc; ++argv;
  if (argc > 0 && **argv == '-' && (*argv)[1] == 's') { src = 1; --argc; ++argv; }
  if (argc < 1) { printf("usage: c4x86 [-s] file ...\n"); return -1; }

  if ((fd = open(*argv, 0)) < 0) { printf("could not open(%s)\n", *argv); return -1; }

  poolsz = 256*1024; // arbitrary size
  if (!(sym = malloc(poolsz))) { printf("could not malloc(%d) symbol area\n", poolsz); return -1; }
  if (!(text = le = e = malloc(poolsz))) { printf("could not malloc(%d) text area\n", poolsz); return -1; }
  if (!(data = malloc(poolsz))) { printf("could not malloc(%d) data area\n", poolsz); return -1; }

  memset(sym,  0, poolsz);
  memset(e,    0, poolsz);
  memset(data, 0, poolsz);

  p = "char else enum if int return sizeof while "
      "open read close printf malloc memset memcmp memcpy mmap dlopen dlsym qsort exit void main";
  i = Char; while (i <= While) { next(); id[Tk] = i++; } // add keywords to symbol table
  i = OPEN; while (i <= EXIT) { next(); id[Class] = Sys; id[Type] = INT; id[Val] = i++; } // add library to symbol table
  next(); id[Tk] = Char; // handle void type
  next(); idmain = id; // keep track of main

  if (!(lp = p = malloc(poolsz))) { printf("could not malloc(%d) source area\n", poolsz); return -1; }
  if ((i = read(fd, p, poolsz-1)) <= 0) { printf("read() returned %d\n", i); return -1; }
  close(fd);
  p[i] = 0;
  if (src) {
    linemap = (char **)(((int)(p + i + 1) & 0xffffff00) + 0x100);
    srcmap = text + (poolsz / 8);
  }

  // parse declarations
  line = 1;
  next();
  while (tk) {
    bt = INT; // basetype
    if (tk == Int) next();
    else if (tk == Char) { next(); bt = CHAR; }
    else if (tk == Enum) {
      next();
      if (tk != '{') next();
      if (tk == '{') {
        next();
        i = 0;
        while (tk != '}') {
          if (tk != Id) { printf("%d: bad enum identifier %d\n", line, tk); return -1; }
          next();
          if (tk == Assign) {
            next();
            if (tk != Num) { printf("%d: bad enum initializer\n", line); return -1; }
            i = ival;
            next();
          }
          id[Class] = Num; id[Type] = INT; id[Val] = i++;
          if (tk == ',') next();
        }
        next();
      }
    }
    while (tk != ';' && tk != '}') {
      ty = bt;
      while (tk == Mul) { next(); ty = ty + PTR; }
      if (tk != Id) { printf("%d: bad global declaration\n", line); return -1; }
      if (id[Class]) { printf("%d: duplicate global definition\n", line); return -1; }
      next();
      id[Type] = ty;
      if (tk == '(') { // function
        id[Class] = Fun;
        id[Val] = (int)(e + 1);
        next(); i = 0;
        while (tk != ')') {
          ty = INT;
          if (tk == Int) next();
          else if (tk == Char) { next(); ty = CHAR; }
          while (tk == Mul) { next(); ty = ty + PTR; }
          if (tk != Id) { printf("%d: bad parameter declaration\n", line); return -1; }
          if (id[Class] == Loc) { printf("%d: duplicate parameter definition\n", line); return -1; }
          id[HClass] = id[Class]; id[Class] = Loc;
          id[HType]  = id[Type];  id[Type] = ty;
          id[HVal]   = id[Val];   id[Val] = i++;
          next();
          if (tk == ',') next();
        }
        next();
        if (tk != '{') { printf("%d: bad function definition\n", line); return -1; }
        loc = ++i;
        next();
        while (tk == Int || tk == Char) {
          bt = (tk == Int) ? INT : CHAR;
          next();
          while (tk != ';') {
            ty = bt;
            while (tk == Mul) { next(); ty = ty + PTR; }
            if (tk != Id) { printf("%d: bad local declaration\n", line); return -1; }
            if (id[Class] == Loc) { printf("%d: duplicate local definition\n", line); return -1; }
            id[HClass] = id[Class]; id[Class] = Loc;
            id[HType]  = id[Type];  id[Type] = ty;
            id[HVal]   = id[Val];   id[Val] = ++i;
            next();
            if (tk == ',') next();
          }
          next();
        }
        *++e = ENT; *++e = i - loc;
        while (tk != '}') stmt();
        *++e = LEV;
        id = sym; // unwind symbol table locals
        while (id[Tk]) {
          if (id[Class] == Loc) {
            id[Class] = id[HClass];
            id[Type] = id[HType];
            id[Val] = id[HVal];
          }
          id = id + Idsz;
        }
      }
      else {
        id[Class] = Glo;
        id[Val] = (int)data;
        data = data + sizeof(int);
      }
      if (tk == ',') next();
    }
    next();
  }

  dl = dlopen(0, RTLD_LAZY | RTLD_GLOBAL); // RTLD_LAZY = 1

  // setup jit memory
  //jitmem = mmap(0, poolsz, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  jitmem = mmap(0, poolsz, 7, 0x1002 | MAP_ANON, -1, 0);
  if (!jitmem) { printf("could not mmap(%d) jit executable memory\n", poolsz); return -1; }

  jitmap = (char **)(jitmem + poolsz / 2);

  // first pass: emit native code
  pc = text + 1; je = jitmem; line = 0;
  while (pc <= e) {
    i = *pc;
    if (src) {
        while (line < srcmap[pc - text]) {
            line++; printf("% 4d | %.*s", line, linemap[line + 1] - linemap[line], linemap[line]);
        }
        printf("0x%05x (%p):\t%8.4s", pc - text, je,
                        &"LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LEV ,LI  ,LC  ,SI  ,SC  ,PSH ,"
                         "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,ADD ,SUB ,MUL ,DIV ,MOD ,"
                         "OPEN,READ,CLOS,PRTF,MALC,MSET,MCMP,MCPY,MMAP,DOPN,DSYM,QSRT,EXIT,"[i * 5]);
        if (i <= ADJ) printf(" 0x%x\n", *(pc + 1)); else printf("\n");
    }
    jitmap[pc - text] = je;  // for later relocation of JMP/JSR/BZ/BNZ
    pc++;
    if (i == LEA) {
      i = 4 * *pc++; if (i < -128 || i > 127) { printf("jit: LEA out of bounds\n"); return -1; }
      *(int*)je = 0x458d; je = je + 2; *je++ = i;  // leal $(4 * n)(%ebp), %eax
    }
    else if (i == ENT) {
      i = 4 * *pc++; if (i < -128 || i > 127) { printf("jit: ENT out of bounds\n"); return -1; }
      *(int *)je = 0xe58955; je = je + 3;  // push %ebp; movl %esp, %ebp
      if (i > 0) { *(int *)je = 0xec83; je = je + 2; *(int*)je++ = i; } // subl $(i*4), %esp
    }
    else if (i == IMM) { *je++ = 0xb8; *(int *)je = *pc++; je = je + 4; } // movl $imm, %eax
    else if (i == ADJ) { i = 4 * *pc++; *(int *)je = 0xc483; je = je + 2; *(int *)je = i; je++; } // addl $(n * 4), %esp
    else if (i == PSH)   *(int *)je++ = 0x50;                    // push %eax
    else if (i == LEV) { *(int *)je = 0xc35dec89; je = je + 4; } // mov %ebp, %esp; pop %ebp; ret
    else if (i == LI)  { *(int *)je = 0x008b;     je = je + 2; } // movl (%eax), %eax
    else if (i == LC)  { *(int *)je = 0x00b60f;   je = je + 3; } // movzbl (%eax), %eax
    else if (i == SI)  { *(int *)je = 0x018959;   je = je + 3; } // pop %ecx; movl %eax, (%ecx)
    else if (i == SC)  { *(int *)je = 0x018859;   je = je + 3; } // pop %ecx; movb %al, (%ecx)
    else if (i == OR)  { *(int *)je = 0xc80959;   je = je + 3; } // pop %ecx; orl %ecx, %eax
    else if (i == XOR) { *(int *)je = 0xc83159;   je = je + 3; } // pop %ecx; xorl %ecx, %eax
    else if (i == AND) { *(int *)je = 0xc82159;   je = je + 3; } // pop %ecx; andl %ecx, %eax
    else if (EQ <= i && i <= GE) {
        *(int*)je=0x0fc13959; je = je + 4; *(int*)je=0x9866c094; // pop %ecx; cmp %ecx, %eax; sete %al; cbw; - EQ
        if      (i == NE)  { *je = 0x95; } // setne %al
        else if (i == LT)  { *je = 0x9c; } // setl %al
        else if (i == GT)  { *je = 0x9f; } // setg %al
        else if (i == LE)  { *je = 0x9e; } // setle %al
        else if (i == GE)  { *je = 0x9d; } // setge %al
        je=je+4; *je++=0x98;  // cwde
    }
    else if (i == SHL) { *(int*)je = 0xe0d39159; je = je + 4; } // pop %ecx; xchg %eax, %ecx; shl %cl, %eax
    else if (i == SHR) { *(int*)je = 0xe8d39159; je = je + 4; } // pop %ecx; xchg %eax, %ecx; shr %cl, %eax
    else if (i == ADD) { *(int*)je = 0xc80159;   je = je + 3; } // pop %ecx; addl %ecx, %eax
    else if (i == SUB) { *(int*)je = 0xc8299159; je = je + 4; } // pop %ecx; xchg %eax, %ecx; subl %ecx, %eax
    else if (i == MUL) { *(int*)je = 0xc1af0f59; je = je + 4; } // pop %ecx; imul %ecx, %eax
    else if (i == DIV) { *(int*)je = 0xf9f79159; je = je + 4; } // pop %ecx; xchg %eax, %ecx; idiv %ecx, %eax
    else if (i == MOD) { *(int*)je = 0xd2319159; je = je + 4; *(int *)je = 0x92f9f7; je = je + 3; }
    else if (i == JMP) { ++pc; *je       = 0xe9;     je = je + 5; } // jmp <off32>
    else if (i == JSR) { ++pc; *je       = 0xe8;     je = je + 5; } // call <off32>
    else if (i == BZ)  { ++pc; *(int*)je = 0x840fc085; je = je + 8; } // test %eax, %eax; jz <off32>
    else if (i == BNZ) { ++pc; *(int*)je = 0x850fc085; je = je + 8; } // test %eax, %eax; jnz <off32>
    else if (i >= OPEN) {
      if      (i == OPEN) tmp = (int)dlsym(dl, "open");
      else if (i == READ) tmp = (int)dlsym(dl, "read");
      else if (i == CLOS) tmp = (int)dlsym(dl, "close");
      else if (i == PRTF) tmp = (int)dlsym(dl, "printf");
      else if (i == MALC) tmp = (int)dlsym(dl, "malloc");
      else if (i == MSET) tmp = (int)dlsym(dl, "memset");
      else if (i == MCMP) tmp = (int)dlsym(dl, "memcmp");
      else if (i == MCPY) tmp = (int)dlsym(dl, "memcpy");
      else if (i == MMAP) tmp = (int)dlsym(dl, "mmap");
      else if (i == DOPN) tmp = (int)dlsym(dl, "dlopen");
      else if (i == DSYM) tmp = (int)dlsym(dl, "dlsym");
      else if (i == QSRT) tmp = (int)dlsym(dl, "qsort");
      else if (i == EXIT) tmp = (int)dlsym(dl, "exit");

      if (*pc++ == ADJ) { i = *pc++; } else { printf("no ADJ after native proc!\n"); exit(2); }

      *je++ = 0xb9; *(int*)je = i << 2; je = je + 4;  // movl $(4 * n), %ecx;
      *(int*)je = 0xce29e689; je = je + 4; // mov %esp, %esi; sub %ecx, %esi;  -- %esi will adjust the stack
      *(int*)je = 0x8302e9c1; je = je + 4; // shr $2, %ecx; and                -- alignment of %esp for OS X
      *(int*)je = 0x895af0e6; je = je + 4; // $0xfffffff0, %esi; pop %edx; mov..
      *(int*)je = 0xe2fc8e54; je = je + 4; // ..%edx, -4(%esi,%ecx,4); loop..  -- reversing args order
      *(int*)je = 0xe8f487f9; je = je + 4; // ..<'pop' offset>; xchg %esi, %esp; call    -- saving old stack in %esi
      *(int*)je = tmp - (int)(je + 4); je = je + 4; // <*tmp offset>;
      *(int*)je = 0xf487; je = je + 2;     // xchg %esi, %esp  -- ADJ, back to old stack without arguments
    }
    else { printf("code generation failed for %d!\n", i); return -1; }
  }

  // second pass, relocation
  pc = text + 1;
  while (pc <= e) {
    je = jitmap[pc - text];
    i = *pc++;
    if (i == JSR || i == JMP || i == BZ || i == BNZ) {
        tmp = (int)jitmap[(int *)*pc++ - text];
        if      (i == JSR || i == JMP) { je = je + 1; *(int*)je = tmp - (int)(je + 4); }
        else if (i == BZ  || i == BNZ) { je = je + 4; *(int*)je = tmp - (int)(je + 4); }
    }
    else if (i < LEV) { ++pc; }
  }

  // run jitted code
  pc = (int *) idmain[Val];
  jitmain = (void *) jitmap[ pc - text ];
  return jitmain(argv, argc); // c4 vm pushes first argument first, unlike cdecl
}

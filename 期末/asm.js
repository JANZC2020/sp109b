var fs = require("fs");
var c  = console;
var file = process.argv[2];

var dest = {
  ""   :0b000,  "M"  :0b001,  "D"  :0b010,
  "MD" :0b011,  "A"  :0b100,  "AM" :0b101,
  "AD" :0b110,  "AMD":0b111
}

var jump = {
  ""   :0b000,  "JGT":0b001,  "JEQ":0b010,
  "JGE":0b011,  "JLT":0b100,  "JNE":0b101,
  "JLE":0b110,  "JMP":0b111
}

var comp = {
  "0"   :0b0101010, "1"   :0b0111111,  "-1"  :0b0111010,
  "D"   :0b0001100, "A"   :0b0110000,  "M"   :0b1110000,
  "!D"  :0b0001101, "!A"  :0b0110001,  "!M"  :0b1110001,
  "-D"  :0b0001111, "-A"  :0b0110011,  "-M"  :0b1110011,
  "D+1" :0b0011111, "A+1" :0b0110111,  "M+1" :0b1110111,
  "D-1" :0b0001110, "A-1" :0b0110010,  "M-1" :0b1110010,
  "D+A" :0b0000010, "D+M" :0b1000010,  "D-A" :0b0010011,
  "D-M" :0b1010011, "A-D" :0b0000111,  "M-D" :0b1000111,
  "D&A" :0b0000000, "D&M" :0b1000000,  "D|A" :0b0010101,
  "D|M" :0b1010101
}

var symTable = {
  "R0"  :0,     "R1"  :1,   "R2"  :2,
  "R3"  :3,     "R4"  :4,   "R5"  :5,
  "R6"  :6,     "R7"  :7,   "R8"  :8,
  "R9"  :9,     "R10" :10,  "R11" :11,
  "R12" :12,    "R13" :13,  "R14" :14,
  "R15" :15,    "SP"  :0,   "LCL" :1,
  "ARG" :2,     "THIS":3,   "THAT":4,
  "KBD" :24576, "SCREEN":16384
};

var symTop = 16;

function addSymbol(symbol) {  // symTop從16開始 (變數增加)
  symTable[symbol] = symTop;
  symTop ++;
} 

asm(file+'.asm', file+'.hack'); // 輸入.asm檔 輸出.hack檔

function asm(asmFile, objFile) {    // asm(輸入, 輸出)
    var asmText = fs.readFileSync(asmFile, "utf8"); // 讀取檔案到 text 字串中  
                                                    // fs.readFileSync 是一模組 但用於較大的檔案用法不優
    var lines   = asmText.split(/\r?\n/);           // /r & /n 都是換行字元 中間的+?是因為用於判斷 winows , linux , mac
                                                    // linux , mac 換行 -> /n       windos 換行 -> /r + /n
    c.log(JSON.stringify(lines, null, 2));          // 印出 方便比對
    round1(lines);                                   // 第一輪 記住所有符號位置
    round2(lines, objFile);                          // 第二輪 開始編碼
}

function parse(line, i) {                               // line代表目前進入的行數 , i代表第幾行
    line.match(/^([^\/]*)(\/.*)?$/);                    // 不要註解中的 第一個 '/' ,             
    line = RegExp.$1.trim();
    if (line.length===0)                                // 若是空行，直接傳回空值
      return null;
    if (line.startsWith("@")) {                         // 若是@則代表一個A指令，但後面可能是符號，則先取出位址在編碼，是數字可直接編碼
      return { type:"A", arg:line.substring(1).trim() } // 因為第0個是@，所以從1開始
    } else if (line.match(/^\(([^\)]+)\)$/)) {          //  \ 代表跳開不比對  , 比對直到遇到 )但不包含 ) , $代表結尾 +代表1次以上
      return { type:"S", symbol:RegExp.$1 }
    } else if (line.match(/^((([AMD]*)=)?([AMD01\+\-\&\|\!]*))(;(\w*))?$/)) {
                                                        //  比對C指令，
                                                        //   [AMD]* AMD 0次或更多                        
                                                        //  (([AMD]*)=)? AMD = 0次或一次
                                                        //                    ([AMD01\+\-\&\|\!]*) 
                                                        //                  A、M、D、0、1、+、-、&、|、!
                                                        //                                (\w*) 比對數字、字母、底線
                                                        // ;後面處理是不是跳躍指令 , 類似一個變數指令
                                                        // A = M
                                                        // D;JGT
                                                        // 只要是?，代表前面的區塊是不一定有的
      return { type:"C", c:RegExp.$4, d:RegExp.$3, j:RegExp.$6 } 
      // 分辨區塊:從左邊開始的大括號'('依順序看過去， ex:([AMD]*) -> 區塊3 , D欄位               
    } else {
      throw "Error: line "+(i+1);
    }
  }
  
  function round1(lines) {
    c.log("============== round1 ================");
    var address = 0;
    for (var i=0; i<lines.length; i++) {
      var p = parse(lines[i], i);               // parse 是把一行解析出哪種指令、欄位值
      if (p===null) continue;                   // 若是空值 則繼續下一行 
      if (p.type === "S") {                     // 若是符號 則加到符號表記住位址
        c.log(" symbol: %s %s", p.symbol, intToStr(address, 4, 10));
        symTable[p.symbol] = address;
        continue;                               // * 遇見符號不能+1 continue讓迴圈回到開始位址

      } else {                                 
       c.log(" p: %j", p);                      // 印出parse指令結果 
      }
      c.log("%s:%s %s", intToStr(i+1, 3, 10), intToStr(address, 4, 10),  lines[i]);
      address = symTop;
      addSymbol(p.arg, address);                //  若看到不是空行 也不是註解 位址就+1 
    }
  }
  
  function round2(lines, objFile) {                            // objfile 是輸出檔
    c.log("============== round2 ================");
    var ws = fs.createWriteStream(objFile);
    ws.once('open', function(fd) {
      var address = 0;                                        // 第一個指令從0開始
      for (var i=0; i<lines.length; i++) {
        var p = parse(lines[i], i);
        if (p===null || p.type === "S") continue;
        var code = toCode(p);                                 // 若進到這，已經是 A 或 C指令 ，直接轉成code並列出
        c.log("%s:%s %s", intToStr(i+1, 3, 10), intToStr(code, 16, 2),  lines[i]);
        ws.write(intToStr(code, 16, 2)+"\n");
  
      }
      ws.end();
    });
  }
  
  function intToStr(num, size, radix) {
    //  c.log(" num="+num);
    var s =  num.toString(radix)+"";           //        toString(想轉成幾進位)
    while (s.length < size) s = "0" + s;
    return s;
  }
  
  function toCode(p) {
    var address; 
    if (p.type === "A") {                           
      if (p.arg.match(/^\d+$/)) {               // 若@後面是數字,呼叫parseInt函數後轉換成數字
        address = parseInt(p.arg);
      } else {                                  // 若是符號，查符號表、取出符號位址
        address = symTable[p.arg];         
      } return address; 
    } else { // if (p.type === "C")
      var d = dest[p.d];
      var c = comp[p.c];
      var j = jump[p.j];
      return 0b111<<13|c<<6|d<<3|j;
            // 先放入 111 並往左移 13 位，再放入C(共7位)往左移6位...以此類推
            // _____________111
            // 111_____________
            // 111___________cx
            // 111cx___________
    }
  }
  
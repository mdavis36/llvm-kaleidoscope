#include <map>
#include <string>
#include <vector>


// ----------------------------------------------------------------------------
// Lexer for Standard Input.
// ----------------------------------------------------------------------------

// The Lexer wil return [0-255] if it is an unknown token, otherwise one of the below.
enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,
};

static std::string IndetifierStr; // Filled if tok_identifier.
static double NumVal;             // Filled if tok_number.

// getTok returns next character from std input.
static int getTok() {
  static int LastChar = ' ';

  // Skip any whitespace.
  while (isspace(LastChar))
    LastChar = getchar();

  // identifier: [a-z,A-Z][a-zA-Z0-9]*
  if (isalpha(LastChar)) {
    IndetifierStr = LastChar;

    while (isalnum(LastChar = getchar()))
      IndetifierStr += LastChar;

    if (IndetifierStr == "def") return tok_def;
    if (IndetifierStr == "extern") return tok_extern;

    return tok_identifier;
  }

  //TODO: This will read 1.23.45.6 as 1.23 - Need to handle errors for this.
  // Number: [0-9.]+
  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), 0);
    return tok_number;
  }

  // Comment Until End Of Line.
  if (LastChar == '#') {
    do {
      LastChar = getchar();
    } while(LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return getTok();
  }

  // Check for the EOF.
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise return the character value.
  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}




// ----------------------------------------------------------------------------
// AST : Abstract Syntax Tree.
// ----------------------------------------------------------------------------

// ExprAST : Base class for al AST Nodes.
class ExprAST {
public:
  virtual ~ExprAST() = default;
};

// NumberExprAST - Expression classs for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
};

// VariableExprAST - Expression class for referencing a variable.
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(std::string const& Name)
    : Name(Name) {}
};

// BinaryExprAST - Expression Class for a binary Operator.
class BinaryExprAST : public ExprAST {
  using expr_t = std::unique_ptr<ExprAST>;

  char Op;
  expr_t LHS,RHS;

public:
  BinaryExprAST(char Op, expr_t LHS, expr_t RHS)
    : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  using expr_t = std::unique_ptr<ExprAST>;
  using args_t = std::vector<expr_t>;

  std::string Callee;
  args_t Args;

public:
  CallExprAST(std::string const& Callee, args_t Args)
    : Callee(Callee), Args(std::move(Args)) {}
};

// PrototypeAST - This class represents the "prototype" for a function,
// which captures its name and its argument names (and implicitly the 
// numer of arguments the function takes).
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(std::string const& Name, std::vector<std::string> Args)
    : Name(Name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
};

// FunctionAST -  This class represents a function definition itself.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
    : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

// ----------------------------------------------------------------------------
// Parser
// ----------------------------------------------------------------------------

static int CurTok;
static int getNextTok() {
  return CurTok = getTok();
}

std::unique_ptr<ExprAST> LogError(const char* Str) {
  fprintf(stderr, "%s\n", Str);
  return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char* Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

// numberexpr : number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextTok();
  return std::move(Result);
}

// parenexpr : '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextTok(); // eat (.
  auto V = ParseExpression();
  if (!V) 
    return nullptr;

  if (CurTok != ')')
    return LogError("Expected ')'");
  getNextTok(); // eat ).

  return V;
}

//identifierexpr
//  : identifier
//  : identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IndetifierStr;

  getNextTok(); // eat Identifier.

  if (CurTok != '(')
    return std::make_unique<VariableExprAST>(IdName);

  // Call.
  getNextTok(); // eat '('.
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;

      if (CurTok != ',')
        return LogError("Expected ')' or ',' in argument list.");

      getNextTok();
    }
  }

  // Eat the ')'.
  getNextTok();

  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}


// primary
//  : identifierexpr
//  : numberexpr
//  : parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch(CurTok) {
  default:
    return LogError("unknown token when expecting an expression.");
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  }
}


// ********************
// BinOP Parsing
// ********************

static std::map<char, int> BinopPrecedence;

static int GetTokPrecedence() {
  if (!isascii(CurTok))
    return -1;

  // Make sure it is a declared binop.
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0) return -1;
  return TokPrec;
}

// binoprhs 
//  : ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinaryOpRHS(int ExprPrec,
                                                 std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();

    if (TokPrec < ExprPrec)
      return LHS;

    int BinOp = CurTok;
    getNextTok();

    auto RHS = ParsePrimary();
    if (!RHS)
      return nullptr;

    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinaryOpRHS(TokPrec+1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS),
                                          std::move(RHS));
  }
}

// expression 
//  - primary binopRHS
static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS)
    return nullptr;

  return ParseBinaryOpRHS(0, std::move(LHS));
}

// prototype
//  : id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier)
    return LogErrorP("Expected function name in prototype.");

  std::string FnName = IndetifierStr;
  getNextTok();

  if (CurTok != '(')
    return LogErrorP("Expected '(' in prototype.");

  // Read the list of argumetnt names.
  std::vector<std::string> ArgNames;
  while (getNextTok() == tok_identifier)
    ArgNames.push_back(IndetifierStr);
  if (CurTok != ')')
    return LogErrorP("Expected ')' in prototype.");

  //success
  getNextTok(); // eat ')'.

  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

//definition : 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextTok(); // Eat 'def'.
  auto Proto = ParsePrototype();
  if (!Proto) return nullptr;

  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));

  return nullptr;
}


//external : 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextTok(); // Eat 'extern'.
  return ParsePrototype();
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    auto Proto = std::make_unique<PrototypeAST>("", std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

static void HandleDefinition() {
  if (ParseDefinition()) {
    fprintf(stderr, "Parsed a function definition.\n");
  } else {
    // Skip token for error recovery.
    getNextTok();
  }
}

static void HandleExtern() {
  if (ParseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
  } else {
    // Skip token for error recovery.
    getNextTok();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (ParseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
  } else {
    // Skip token for error recovery.
    getNextTok();
  }
}

static void MainLoop() {
  while (true) {
    fprintf(stderr, "ready> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // Ignore top level semicolons.
      getNextTok();
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

int main() {

  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 30;
  BinopPrecedence['*'] = 40;

  fprintf(stderr, "ready> ");
  getNextTok();

  MainLoop();

  return 0;
}

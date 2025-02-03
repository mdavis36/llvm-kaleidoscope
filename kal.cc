#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#include <map>
#include <string>
#include <vector>

using namespace llvm;

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
  virtual Value *codegen() = 0;
};

// NumberExprAST - Expression classs for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(double Val) : Val(Val) {}
  Value *codegen() override;
};

// VariableExprAST - Expression class for referencing a variable.
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(std::string const& Name)
    : Name(Name) {}
  Value *codegen() override;
};

// BinaryExprAST - Expression Class for a binary Operator.
class BinaryExprAST : public ExprAST {
  using expr_t = std::unique_ptr<ExprAST>;

  char Op;
  expr_t LHS,RHS;

public:
  BinaryExprAST(char Op, expr_t LHS, expr_t RHS)
    : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
  Value *codegen() override;
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
  Value *codegen() override;
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
  Function* codegen();
};

// FunctionAST -  This class represents a function definition itself.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
    : Proto(std::move(Proto)), Body(std::move(Body)) {}
  Function *codegen();
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

// ----------------------------------------------------------------------------
// CodeGen.
// ----------------------------------------------------------------------------

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value*> NamedValues;

Value *LogErrorV(char const* Str) {
  LogError(Str);
  return nullptr;
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  Value *V = NamedValues[Name];
  if (!V)
    LogErrorV("Unknown variable name");
  return V;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;

  switch (Op) {
  case '+':
    return Builder->CreateFAdd(L,R, "addtmp");
  case '-':
    return Builder->CreateFSub(L,R, "subtmp");
  case '*':
    return Builder->CreateFMul(L,R, "multmp");
  case '<':
    L =  Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext),
                                "booltmp");
  default:
    return LogErrorV("invalid binary operator");
  }
}

Value* CallExprAST::codegen() {
  Function* CalleeF = TheModule->getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function referenced");

  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("Incorrect # arguments passed");

  std::vector<Value*> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen());
    if (!ArgsV.back())
      return nullptr;
  }

  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Function* PrototypeAST::codegen() {
  std::vector<Type*> Doubles(Args.size(),
                              Type::getDoubleTy(*TheContext));
  FunctionType* FT =
    FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

  Function* F =
    Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);

  return F;
}

Function* FunctionAST::codegen() {
  Function* TheFunction = TheModule->getFunction(Proto->getName());

  if (!TheFunction)
    TheFunction = Proto->codegen();

  if (!TheFunction)
    return nullptr;

  if (!TheFunction->empty())
    return (Function*)LogErrorV("Function cannot be redefined.");

  BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  NamedValues.clear();
  for (auto & Arg : TheFunction->args())
    NamedValues[std::string(Arg.getName())] = &Arg;

  if (Value* RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);

    verifyFunction(*TheFunction);

    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}

// ----------------------------------------------------------------------------
// Top Level Parsing
// ----------------------------------------------------------------------------

static void InitializeModule() {
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("my cool jit", *TheContext);

  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition:");
      FnIR->print(errs());
      fprintf(stderr, "\n");
    }
  } else {
    // Skip token for error recovery.
    getNextTok();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (auto *FnIR = ProtoAST->codegen()) {
      fprintf(stderr, "Read extern:");
      FnIR->print(errs());
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "Parsed an extern\n");
  } else {
    // Skip token for error recovery.
    getNextTok();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read top-level experession:");
      FnIR->print(errs());
      fprintf(stderr, "\n");

      FnIR->eraseFromParent();
    }
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

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

int main() {
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 30;
  BinopPrecedence['*'] = 40;

  fprintf(stderr, "ready> ");
  getNextTok();

  InitializeModule();

  MainLoop();

  TheModule->print(errs(), nullptr);

  return 0;
}

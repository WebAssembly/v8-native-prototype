// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#include "src/wasm/asm-wasm-builder.h"
#include "src/wasm/wasm-macro-gen.h"
#include "src/wasm/wasm-opcodes.h"

#include "src/ast.h"
#include "src/codegen.h"
#include "src/scopes.h"
#include "src/type-cache.h"

namespace v8 {
namespace internal {
namespace wasm {

#define RECURSE(call)            \
  do {                           \
    DCHECK(!HasStackOverflow()); \
    call;                        \
    if (HasStackOverflow())      \
      return;                    \
  } while (false)


class AsmWasmBuilderImpl : public AstVisitor {
 public:
  AsmWasmBuilderImpl(Isolate* isolate, Zone* zone, FunctionLiteral* literal)
      : local_variables_(HashMap::PointersMatch,
                         ZoneHashMap::kDefaultHashMapCapacity,
                         ZoneAllocationPolicy(zone)),
        functions_(HashMap::PointersMatch,
                   ZoneHashMap::kDefaultHashMapCapacity,
                   ZoneAllocationPolicy(zone)),
        in_function_(false),
        is_set_op_(false),
        marking_exported(false),
        builder_(new (zone) WasmModuleBuilder(zone)),
        current_function_builder_(NULL),
        literal_(literal),
        isolate_(isolate),
        zone_(zone),
        cache_(TypeCache::Get()),
        block_depth_(0),
        breakable_blocks_(zone) {
    InitializeAstVisitor(isolate);
  }

  void Compile() { RECURSE(VisitFunctionLiteral(literal_)); }

  void VisitVariableDeclaration(VariableDeclaration* decl) {}

  void VisitFunctionDeclaration(FunctionDeclaration* decl) {
    DCHECK(!in_function_);
    DCHECK(current_function_builder_ == NULL);
    DCHECK(block_depth_ == 0);
    uint16_t index = LookupOrInsertFunction(decl->proxy()->var());
    current_function_builder_ = builder_->FunctionAt(index);
    in_function_ = true;
    RECURSE(Visit(decl->fun()));
    in_function_ = false;
    current_function_builder_ = NULL;
    local_variables_.Clear();
  }

  void VisitImportDeclaration(ImportDeclaration* decl) {}

  void VisitExportDeclaration(ExportDeclaration* decl) {}

  void VisitStatements(ZoneList<Statement*>* stmts) {
    if (in_function_) {
      current_function_builder_->AppendCode(kExprBlock, false);
      current_function_builder_->AppendCode(
          static_cast<byte>(stmts->length()), false);
    }

    for (int i = 0; i < stmts->length(); ++i) {
      Statement* stmt = stmts->at(i);
      RECURSE(Visit(stmt));
      if (stmt->IsJump())
        break;
    }
  }

  void VisitBlock(Block* stmt) {
    DCHECK(in_function_);
    block_depth_++;
    breakable_blocks_.push_back(
        std::make_pair(stmt->AsBreakableStatement(), false));
    RECURSE(VisitStatements(stmt->statements()));
    block_depth_--;
    breakable_blocks_.pop_back();
  }

  void VisitExpressionStatement(ExpressionStatement* stmt) {
    RECURSE(Visit(stmt->expression()));
  }

  void VisitEmptyStatement(EmptyStatement* stmt) {}

  void VisitEmptyParentheses(EmptyParentheses* paren) { UNREACHABLE(); }

  void VisitIfStatement(IfStatement* stmt) {
    DCHECK(in_function_);
    if(stmt->HasElseStatement()) {
      current_function_builder_->AppendCode(kExprIfThen, false);
    } else {
      current_function_builder_->AppendCode(kExprIf, false);
    }
    RECURSE(Visit(stmt->condition()));
    if (stmt->HasThenStatement()) {
      RECURSE(Visit(stmt->then_statement()));
    } else {
      current_function_builder_->AppendCode(kExprNop, false);
    }
    if (stmt->HasElseStatement()) {
      RECURSE(Visit(stmt->else_statement()));
    }
  }

  void VisitContinueStatement(ContinueStatement* stmt) {
    DCHECK(in_function_);
    int i = static_cast<int>(breakable_blocks_.size()) - 1;
    int block_distance = 0;
    for (; i >= 0; i--) {
      auto elem = breakable_blocks_.at(i);
      if (elem.first == stmt->target()) {
        DCHECK(elem.second);
        break;
      } else if (elem.second) {
        block_distance += 2;
      } else {
        block_distance += 1;
      }
    }
    DCHECK(i >= 0);
    current_function_builder_->AppendCode(kExprBr, false);
    current_function_builder_->AppendCode(block_distance, false);
    current_function_builder_->AppendCode(kExprNop, false);
  }

  void VisitBreakStatement(BreakStatement* stmt) {
    DCHECK(in_function_);
    int i = static_cast<int>(breakable_blocks_.size()) - 1;
    int block_distance = 0;
    for (; i >= 0; i--) {
      auto elem = breakable_blocks_.at(i);
      if (elem.first == stmt->target()) {
        if (elem.second) {
          block_distance++;
        }
        break;
      } else if (elem.second) {
        block_distance += 2;
      } else {
        block_distance += 1;
      }
    }
    DCHECK(i >= 0);
    current_function_builder_->AppendCode(kExprBr, false);
    current_function_builder_->AppendCode(block_distance, false);
    current_function_builder_->AppendCode(kExprNop, false);
  }

  void VisitReturnStatement(ReturnStatement* stmt) {
    if (in_function_) {
      current_function_builder_->AppendCode(kExprBr, false);
      current_function_builder_->AppendCode(block_depth_, false);
    } else {
      marking_exported = true;
    }
    RECURSE(Visit(stmt->expression()));
    if (!in_function_) {
      marking_exported = false;
    }
  }

  void VisitWithStatement(WithStatement* stmt) {
    RECURSE(stmt->expression());
    RECURSE(stmt->statement());
  }

  void VisitSwitchStatement(SwitchStatement* stmt) {
    RECURSE(Visit(stmt->tag()));

    ZoneList<CaseClause*>* clauses = stmt->cases();

    for (int i = 0; i < clauses->length(); ++i) {
      CaseClause* clause = clauses->at(i);
      if (!clause->is_default()) {
        Expression* label = clause->label();
        RECURSE(Visit(label));
      }
      ZoneList<Statement*>* stmts = clause->statements();
      RECURSE(VisitStatements(stmts));
    }
  }

  void VisitCaseClause(CaseClause* clause) { UNREACHABLE(); }

  void VisitDoWhileStatement(DoWhileStatement* stmt) {
    RECURSE(Visit(stmt->body()));
    RECURSE(Visit(stmt->cond()));
  }

  void VisitWhileStatement(WhileStatement* stmt) {
    DCHECK(in_function_);
    current_function_builder_->AppendCode(kExprLoop, false);
    current_function_builder_->AppendCode(1, false);
    block_depth_ += 2;
    breakable_blocks_.push_back(
        std::make_pair(stmt->AsBreakableStatement(), true));
    current_function_builder_->AppendCode(kExprIf, false);
    RECURSE(Visit(stmt->cond()));
    current_function_builder_->AppendCode(kExprBr, false);
    current_function_builder_->AppendCode(0, false);
    RECURSE(Visit(stmt->body()));
    block_depth_ -= 2;
    breakable_blocks_.pop_back();
  }

  void VisitForStatement(ForStatement* stmt) {
    if (stmt->init() != NULL) {
      RECURSE(Visit(stmt->init()));
    }
    if (stmt->cond() != NULL) {
      RECURSE(Visit(stmt->cond()));
    }
    if (stmt->next() != NULL) {
      RECURSE(Visit(stmt->next()));
    }
    RECURSE(Visit(stmt->body()));
  }

  void VisitForInStatement(ForInStatement* stmt) {
    RECURSE(Visit(stmt->enumerable()));
    RECURSE(Visit(stmt->body()));
  }

  void VisitForOfStatement(ForOfStatement* stmt) {
    RECURSE(Visit(stmt->iterable()));
    RECURSE(Visit(stmt->body()));
  }

  void VisitTryCatchStatement(TryCatchStatement* stmt) {
    RECURSE(Visit(stmt->try_block()));
    RECURSE(Visit(stmt->catch_block()));
  }

  void VisitTryFinallyStatement(TryFinallyStatement* stmt) {
    RECURSE(Visit(stmt->try_block()));
    RECURSE(Visit(stmt->finally_block()));
  }

  void VisitDebuggerStatement(DebuggerStatement* stmt) {}

  void VisitFunctionLiteral(FunctionLiteral* expr) {
    Scope* scope = expr->scope();
    if (in_function_) {
      if (expr->bounds().lower->IsFunction()) {
        Type::FunctionType* func_type = expr->bounds().lower->AsFunction();
        LocalType return_type = TypeFrom(func_type->Result());
        current_function_builder_->ReturnType(return_type);
        for(int i = 0; i < expr->parameter_count(); i++) {
          LocalType type = TypeFrom(func_type->Parameter(i));
          DCHECK(type != kAstStmt);
          LookupOrInsertLocal(scope->parameter(i), type);
        }
      } else {
        UNREACHABLE();
      }
    }
    RECURSE(VisitDeclarations(scope->declarations()));
    RECURSE(VisitStatements(expr->body()));
  }

  void VisitNativeFunctionLiteral(NativeFunctionLiteral* expr) {}

  void VisitConditional(Conditional* expr) {
    RECURSE(Visit(expr->condition()));
    RECURSE(Visit(expr->then_expression()));
    RECURSE(Visit(expr->else_expression()));
  }

  void VisitVariableProxy(VariableProxy* expr) {
    if (in_function_) {
      Variable* var = expr->var();
      if (var->is_function()) {
        std::vector<uint8_t> index =
            UnsignedLEB128From(LookupOrInsertFunction(var));
        current_function_builder_->AddBody(index.data(),
                                           static_cast<uint32_t>(index.size()));
      } else {
        if (!is_set_op_) {
          current_function_builder_->AppendCode(kExprGetLocal, false);
        }
        LocalType var_type = TypeOf(expr);
        DCHECK(var_type != kAstStmt);
        std::vector<uint8_t> index =
            UnsignedLEB128From(LookupOrInsertLocal(var, var_type));
        uint32_t pos_of_index[1] = {0};
        current_function_builder_->AddBody(
            index.data(), static_cast<uint32_t>(index.size()), pos_of_index, 1);
      }
    } else if (marking_exported) {
      Variable* var = expr->var();
      if (var->is_function()) {
        uint16_t index = LookupOrInsertFunction(var);
        builder_->FunctionAt(index)->Exported(1);
      }
    }
  }

  void VisitLiteral(Literal* expr) {
    if (in_function_) {
      if (expr->raw_value()->IsNumber()) {
        LocalType type = TypeOf(expr);
        switch (type) {
          case kAstI32: {
            int val = static_cast<int>(expr->raw_value()->AsNumber());
            byte code[] = {WASM_I32(val)};
            current_function_builder_->AddBody(code, sizeof(code));
          }
          break;
          case kAstF32: {
            float val = static_cast<float>(expr->raw_value()->AsNumber());
            byte code[] = {WASM_F32(val)};
            current_function_builder_->AddBody(code, sizeof(code));
          }
          break;
          case kAstF64: {
            double val = static_cast<double>(expr->raw_value()->AsNumber());
            byte code[] = {WASM_F64(val)};
            current_function_builder_->AddBody(code, sizeof(code));
          }
          break;
          default:
            UNREACHABLE();
        }
      }
    }
  }

  void VisitRegExpLiteral(RegExpLiteral* expr) {}

  void VisitObjectLiteral(ObjectLiteral* expr) {
    ZoneList<ObjectLiteralProperty*>* props = expr->properties();
    for (int i = 0; i < props->length(); ++i) {
      ObjectLiteralProperty* prop = props->at(i);
      RECURSE(Visit(prop->value()));
    }
  }

  void VisitArrayLiteral(ArrayLiteral* expr) {
    ZoneList<Expression*>* values = expr->values();
    for (int i = 0; i < values->length(); ++i) {
      Expression* value = values->at(i);
      RECURSE(Visit(value));
    }
  }

  void VisitAssignment(Assignment* expr) {
    Property* property = expr->target()->AsProperty();
    LhsKind assign_type = Property::GetAssignType(property);

    // Evaluate LHS expression.
    switch (assign_type) {
      case VARIABLE:
        DCHECK(in_function_);
        current_function_builder_->AppendCode(kExprSetLocal, false);
        is_set_op_ = true;
        RECURSE(Visit(expr->target()));
        is_set_op_ = false;
        RECURSE(Visit(expr->value()));
        break;
      default:
        UNREACHABLE();
    }
  }

  void VisitYield(Yield* expr) {
    RECURSE(Visit(expr->generator_object()));
    RECURSE(Visit(expr->expression()));
  }

  void VisitThrow(Throw* expr) { RECURSE(Visit(expr->exception())); }

  void VisitProperty(Property* expr) {
    RECURSE(Visit(expr->obj()));
    RECURSE(Visit(expr->key()));
  }

  void VisitCall(Call* expr) {
    Call::CallType call_type = expr->GetCallType(isolate_);
    switch (call_type) {
      case Call::OTHER_CALL: {
        DCHECK(in_function_);
        current_function_builder_->AppendCode(kExprCallFunction, false);
        RECURSE(Visit(expr->expression()));
        ZoneList<Expression*>* args = expr->arguments();
        for (int i = 0; i < args->length(); ++i) {
          Expression* arg = args->at(i);
          RECURSE(Visit(arg));
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  void VisitCallNew(CallNew* expr) { UNREACHABLE(); }

  void VisitCallRuntime(CallRuntime* expr) { UNREACHABLE(); }

  void VisitUnaryOperation(UnaryOperation* expr) {
    switch (expr->op()) {
      case Token::NOT: {
        int type = TypeIndexOf(expr->expression());
        DCHECK(type == 0);
        current_function_builder_->AppendCode(kExprBoolNot, false);
      }
      break;
      default:
        UNREACHABLE();
    }
    RECURSE(Visit(expr->expression()));
  }

  void VisitCountOperation(CountOperation* expr) {
    RECURSE(Visit(expr->expression()));
  }

#define NON_SIGNED_BINOP(op)                                          \
  static WasmOpcode opcodes[] = {kExprI32##op, kExprI32##op,          \
                                 kExprF32##op, kExprF64##op}

#define SIGNED_BINOP(op)                                              \
  static WasmOpcode opcodes[] = {kExprI32##op##S, kExprI32##op##U,    \
                                 kExprF32##op, kExprF64##op}

#define NON_SIGNED_INT_BINOP(op)                                      \
  static WasmOpcode opcodes[] = {kExprI32##op, kExprI32##op}

#define BINOP_CASE(token, op, V, ignore_sign)                         \
  case token: {                                                       \
    V(op);                                                            \
    int type = TypeIndexOf(expr->left(), expr->right(), ignore_sign); \
    current_function_builder_->AppendCode(opcodes[type], false);      \
  }                                                                   \
  break

  void VisitBinaryOperation(BinaryOperation* expr) {
    switch (expr->op()) {
      BINOP_CASE(Token::ADD, Add, NON_SIGNED_BINOP, true);
      BINOP_CASE(Token::SUB, Sub, NON_SIGNED_BINOP, true);
      BINOP_CASE(Token::MUL, Mul, NON_SIGNED_BINOP, true);
      BINOP_CASE(Token::DIV, Div, SIGNED_BINOP, false);
      BINOP_CASE(Token::BIT_OR, Ior, NON_SIGNED_INT_BINOP, true);
      BINOP_CASE(Token::BIT_XOR, Xor, NON_SIGNED_INT_BINOP, true);
      BINOP_CASE(Token::SHL, Shl, NON_SIGNED_INT_BINOP, true);
      BINOP_CASE(Token::SAR, ShrS, NON_SIGNED_INT_BINOP, true);
      BINOP_CASE(Token::SHR, ShrU, NON_SIGNED_INT_BINOP, true);
      case Token::MOD: {
        UNREACHABLE();
      }
      break;
      default:
        UNREACHABLE();
    }
    RECURSE(Visit(expr->left()));
    RECURSE(Visit(expr->right()));
  }

  void VisitCompareOperation(CompareOperation* expr) {
    switch (expr->op()) {
      BINOP_CASE(Token::EQ, Eq, NON_SIGNED_BINOP, false);
      BINOP_CASE(Token::LT, Lt, SIGNED_BINOP, false);
      BINOP_CASE(Token::LTE, Le, SIGNED_BINOP, false);
      BINOP_CASE(Token::GT, Gt, SIGNED_BINOP, false);
      BINOP_CASE(Token::GTE, Ge, SIGNED_BINOP, false);
      default:
        UNREACHABLE();
    }
    RECURSE(Visit(expr->left()));
    RECURSE(Visit(expr->right()));
  }

#undef BINOP_CASE
#undef NON_SIGNED_INT_BINOP
#undef SIGNED_BINOP
#undef NON_SIGNED_BINOP

  int TypeIndexOf(Expression* left, Expression* right, bool ignore_sign) {
    int left_index = TypeIndexOf(left);
    int right_index = TypeIndexOf(right);
    DCHECK((left_index == right_index) ||
        (ignore_sign && (left_index <= 1) && (right_index <= 1)));
    return left_index;
  }

  int TypeIndexOf(Expression* expr) {
    DCHECK(expr->bounds().lower == expr->bounds().upper);
    TypeImpl<ZoneTypeConfig>* type = expr->bounds().lower;
    if (type->Is(cache_.kAsmSigned)) {
      return 0;
    } else if (type->Is(cache_.kAsmUnsigned)) {
      return 1;
    } else if (type->Is(cache_.kAsmInt)) {
      return 0;
    } else if (type->Is(cache_.kAsmFloat)) {
      return 2;
    } else if (type->Is(cache_.kAsmDouble)) {
      return 3;
    } else {
      UNREACHABLE();
    }
  }

#undef CASE
#undef NON_SIGNED_INT
#undef SIGNED
#undef NON_SIGNED

  void VisitThisFunction(ThisFunction* expr) {}

  void VisitDeclarations(ZoneList<Declaration*>* decls) {
    for (int i = 0; i < decls->length(); ++i) {
      Declaration* decl = decls->at(i);
      RECURSE(Visit(decl));
    }
  }

  void VisitClassLiteral(ClassLiteral* expr) {}

  void VisitSpread(Spread* expr) {}

  void VisitSuperPropertyReference(SuperPropertyReference* expr) {}

  void VisitSuperCallReference(SuperCallReference* expr) {}

  void VisitSloppyBlockFunctionStatement(SloppyBlockFunctionStatement* expr) {}

  void VisitDoExpression(DoExpression* expr) {}

  struct IndexContainer : public ZoneObject {
    uint16_t index;
  };

  uint16_t LookupOrInsertLocal(Variable* v, LocalType type) {
    DCHECK(current_function_builder_ != NULL);
    ZoneHashMap::Entry* entry =
        local_variables_.Lookup(v, ComputePointerHash(v));
    if (entry == NULL) {
      uint16_t index;
      if (v->IsParameter()) {
        index = current_function_builder_->AddParam(type);
      } else {
        index = current_function_builder_->AddLocal(type);
      }
      IndexContainer* container = new (zone()) IndexContainer();
      container->index = index;
      entry = local_variables_.LookupOrInsert(v, ComputePointerHash(v),
                                              ZoneAllocationPolicy(zone()));
      entry->value = container;
    }
    return (reinterpret_cast<IndexContainer*>(entry->value))->index;
  }

  uint16_t LookupOrInsertFunction(Variable* v) {
    DCHECK(builder_ != NULL);
    ZoneHashMap::Entry* entry = functions_.Lookup(v, ComputePointerHash(v));
    if (entry == NULL) {
      uint16_t index = builder_->AddFunction();
      IndexContainer* container = new (zone()) IndexContainer();
      container->index = index;
      entry = functions_.LookupOrInsert(v, ComputePointerHash(v),
                                        ZoneAllocationPolicy(zone()));
      entry->value = container;
    }
    return (reinterpret_cast<IndexContainer*>(entry->value))->index;
  }

  LocalType TypeOf(Expression* expr) {
    DCHECK(expr->bounds().lower == expr->bounds().upper);
    return TypeFrom(expr->bounds().lower);
  }

  LocalType TypeFrom(TypeImpl<ZoneTypeConfig>* type) {
    if (type->Is(cache_.kAsmInt)) {
      return kAstI32;
    } else if (type->Is(cache_.kAsmFloat)) {
      return kAstF32;
    } else if (type->Is(cache_.kAsmDouble)) {
      return kAstF64;
    } else {
      return kAstStmt;
    }
  }

  Zone* zone() { return zone_; }

  ZoneHashMap local_variables_;
  ZoneHashMap functions_;
  bool in_function_;
  bool is_set_op_;
  bool marking_exported;
  WasmModuleBuilder* builder_;
  WasmFunctionBuilder* current_function_builder_;
  FunctionLiteral* literal_;
  Isolate* isolate_;
  Zone* zone_;
  TypeCache const& cache_;
  int block_depth_;
  ZoneVector<std::pair<BreakableStatement*, bool>> breakable_blocks_;

  DEFINE_AST_VISITOR_SUBCLASS_MEMBERS();
  DISALLOW_COPY_AND_ASSIGN(AsmWasmBuilderImpl);
};

AsmWasmBuilder::AsmWasmBuilder(Isolate* isolate,
                               Zone* zone,
                               FunctionLiteral* literal)
    : isolate_(isolate), zone_(zone), literal_(literal) {
}

/*TODO: probably should take zone (to write wasm to) as input so that zone in
  constructor may be thrown away once wasm module is written */
WasmModuleIndex* AsmWasmBuilder::Run() {
  AsmWasmBuilderImpl impl(isolate_, zone_, literal_);
  impl.Compile();
  WasmModuleWriter* writer = impl.builder_->Build(zone_);
  return writer->WriteTo(zone_);
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

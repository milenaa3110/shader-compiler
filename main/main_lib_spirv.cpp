// main_lib_spirv.cpp — irgen_spirv: compile .src fragment shaders to Vulkan SPIR-V.
//
// Strategy: parse the .src file via the custom frontend (lexer + parser + AST),
// then walk the AST to emit valid GLSL 450 source, and compile to SPIR-V with
// glslangValidator.  This uses the same lexer/parser/AST as irgen_riscv.
//
// Usage:
//   ./irgen_spirv <output.spv> < shader_fs.src    → output.spv (defaults to module.spv)
//
// Requires:  sudo apt install glslang-tools

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"
#include "../lexer/lexer.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>

extern int CurTok;
int getNextToken();

using namespace llvm;

static void InitializeModule() {
    Context   = std::make_unique<llvm::LLVMContext>();
    TheModule = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder   = std::make_unique<llvm::IRBuilder<>>(*Context);
}

// ─────────────────────────────────────────────────────────────────────────────
// GLSL AST emitter
// ─────────────────────────────────────────────────────────────────────────────

// Format a double as a GLSL float literal (always has a decimal point).
static std::string fmtNum(double v) {
    if (v == (long long)v && v >= -1e14 && v <= 1e14) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld.0", (long long)v);
        return buf;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.8g", v);
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
    return s;
}

static std::string tokToOpStr(int op) {
    switch (op) {
        case tok_plus:          return "+";
        case tok_minus:         return "-";
        case tok_multiply:      return "*";
        case tok_divide:        return "/";
        case tok_greater:       return ">";
        case tok_greater_equal: return ">=";
        case tok_less:          return "<";
        case tok_less_equal:    return "<=";
        case tok_equal:         return "==";
        case tok_not_equal:     return "!=";
        case tok_and:           return "&&";
        case tok_or:            return "||";
        default:                return "?";
    }
}

// Forward declarations
static std::string emitExpr(const ExprAST* node);
static void        emitStmt(const ExprAST* node, std::string& out, int indent);

static std::string emitExpr(const ExprAST* node) {
    if (!node) return "0.0";

    if (auto* n = dynamic_cast<const NumberExprAST*>(node))
        return fmtNum(n->Val);

    if (auto* n = dynamic_cast<const BooleanExprAST*>(node))
        return n->Val ? "true" : "false";

    if (auto* n = dynamic_cast<const VariableExprAST*>(node)) {
        // Vulkan vertex stage uses gl_VertexIndex instead of OpenGL's gl_VertexID
        if (n->Name == "gl_VertexID") return "gl_VertexIndex";
        return n->Name;
    }

    if (auto* n = dynamic_cast<const UnaryExprAST*>(node)) {
        std::string op;
        if      (n->Op == tok_minus || n->Op == '-') op = "-";
        else if (n->Op == tok_not   || n->Op == '!') op = "!";
        else if (n->Op == tok_plus  || n->Op == '+') op = "+";
        return op + "(" + emitExpr(n->Operand.get()) + ")";
    }

    if (auto* n = dynamic_cast<const BinaryExprAST*>(node))
        return "(" + emitExpr(n->LHS.get()) + " " + tokToOpStr(n->Op) + " " +
               emitExpr(n->RHS.get()) + ")";

    if (auto* n = dynamic_cast<const TernaryExprAST*>(node))
        return "(" + emitExpr(n->Cond.get()) + " ? " +
               emitExpr(n->ThenExpr.get()) + " : " +
               emitExpr(n->ElseExpr.get()) + ")";

    if (auto* n = dynamic_cast<const CallExprAST*>(node)) {
        std::string s = n->Callee + "(";
        for (size_t i = 0; i < n->Args.size(); ++i) {
            if (i > 0) s += ", ";
            s += emitExpr(n->Args[i].get());
        }
        return s + ")";
    }

    if (auto* n = dynamic_cast<const MemberAccessExprAST*>(node))
        return emitExpr(n->Object.get()) + "." + n->Member;

    if (auto* n = dynamic_cast<const MatrixAccessExprAST*>(node)) {
        std::string s = emitExpr(n->Object.get()) + "[" + emitExpr(n->Index.get()) + "]";
        if (n->Index2) s += "[" + emitExpr(n->Index2.get()) + "]";
        return s;
    }

    if (auto* n = dynamic_cast<const AssignmentExprAST*>(node)) {
        std::string s;
        if (!n->VarType.empty()) s = n->VarType + " ";
        return s + n->VarName + " = " + emitExpr(n->Init.get());
    }

    if (auto* n = dynamic_cast<const MemberAssignmentExprAST*>(node))
        return emitExpr(n->Object.get()) + "." + n->Member + " = " +
               emitExpr(n->Init.get());

    if (auto* n = dynamic_cast<const MatrixAssignmentExprAST*>(node)) {
        std::string s = emitExpr(n->Object.get()) + "[" + emitExpr(n->Index.get()) + "]";
        if (n->Index2) s += "[" + emitExpr(n->Index2.get()) + "]";
        return s + " = " + emitExpr(n->RHS.get());
    }

    if (auto* n = dynamic_cast<const ArrayDeclExprAST*>(node)) {
        std::string s = n->ElementType + " " + n->Name +
                        "[" + std::to_string(n->Size) + "]";
        if (n->Init) {
            if (auto* ai = dynamic_cast<const ArrayInitExprAST*>(n->Init.get())) {
                // GLSL array constructor: float[N](v0, v1, ...)
                s += " = " + n->ElementType + "[" + std::to_string(n->Size) + "](";
                for (size_t i = 0; i < ai->Elements.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += emitExpr(ai->Elements[i].get());
                }
                s += ")";
            }
        }
        return s;
    }

    return "/* unknown_expr */";
}

static std::string indStr(int n) { return std::string(n * 4, ' '); }

// Emit a block: writes "{\n  stmt1;\n  stmt2;\n<outerInd>}"
static void emitBlock(const BlockExprAST* block, std::string& out, int outerIndent) {
    out += "{\n";
    for (const auto& s : block->Statements)
        emitStmt(s.get(), out, outerIndent + 1);
    out += indStr(outerIndent) + "}";
}

static void emitStmt(const ExprAST* node, std::string& out, int indent) {
    if (!node) return;
    std::string ind = indStr(indent);

    if (auto* b = dynamic_cast<const BlockExprAST*>(node)) {
        out += ind;
        emitBlock(b, out, indent);
        out += "\n";
        return;
    }

    if (auto* n = dynamic_cast<const ReturnStmtAST*>(node)) {
        out += ind + "return";
        if (n->Expr) out += " " + emitExpr(n->Expr.get());
        out += ";\n";
        return;
    }

    if (auto* n = dynamic_cast<const IfExprAST*>(node)) {
        out += ind + "if (" + emitExpr(n->Condition.get()) + ") ";
        if (auto* b = dynamic_cast<const BlockExprAST*>(n->ThenExpr.get())) {
            emitBlock(b, out, indent);
        } else {
            out += "{\n";
            emitStmt(n->ThenExpr.get(), out, indent + 1);
            out += ind + "}";
        }
        if (n->ElseExpr) {
            out += " else ";
            if (dynamic_cast<const IfExprAST*>(n->ElseExpr.get())) {
                // else-if: emit inline (the if emitStmt handles its own indentation)
                out += "\n";
                emitStmt(n->ElseExpr.get(), out, indent);
                return;
            } else if (auto* b = dynamic_cast<const BlockExprAST*>(n->ElseExpr.get())) {
                emitBlock(b, out, indent);
            } else {
                out += "{\n";
                emitStmt(n->ElseExpr.get(), out, indent + 1);
                out += ind + "}";
            }
        }
        out += "\n";
        return;
    }

    if (auto* n = dynamic_cast<const WhileExprAST*>(node)) {
        out += ind + "while (" + emitExpr(n->Condition.get()) + ") ";
        if (auto* b = dynamic_cast<const BlockExprAST*>(n->Body.get())) {
            emitBlock(b, out, indent);
        } else {
            out += "{\n";
            emitStmt(n->Body.get(), out, indent + 1);
            out += ind + "}";
        }
        out += "\n";
        return;
    }

    if (auto* n = dynamic_cast<const ForExprAST*>(node)) {
        out += ind + "for (";
        if (n->Init) out += emitExpr(n->Init.get());
        out += "; ";
        if (n->Condition) out += emitExpr(n->Condition.get());
        out += "; ";
        if (n->Increment) out += emitExpr(n->Increment.get());
        out += ") ";
        if (auto* b = dynamic_cast<const BlockExprAST*>(n->Body.get())) {
            emitBlock(b, out, indent);
        } else {
            out += "{\n";
            emitStmt(n->Body.get(), out, indent + 1);
            out += ind + "}";
        }
        out += "\n";
        return;
    }

    if (dynamic_cast<const BreakStmtAST*>(node))    { out += ind + "break;\n";    return; }
    if (dynamic_cast<const ContinueStmtAST*>(node)) { out += ind + "continue;\n"; return; }
    if (dynamic_cast<const DiscardStmtAST*>(node))  { out += ind + "discard;\n";  return; }

    // Expression statement (assignment, call, etc.)
    out += ind + emitExpr(node) + ";\n";
}

// Returns true for opaque sampler/image types that must be bound as descriptors,
// not packed into a push_constant block.
static bool isSamplerOrImage(const std::string& t) {
    return t == "sampler2D"   || t == "sampler3D" ||
           t == "samplerCube" || t == "image2D";
}

// Generate GLSL 450 shader text from the parsed AST nodes.
// Works for both vertex and fragment stages.
static std::string generateGLSL(const std::vector<std::unique_ptr<ExprAST>>& nodes) {
    std::string out;
    out += "#version 450\n\n";

    // Collect declarations — separate samplers from scalar/vector uniforms
    std::vector<std::pair<std::string,std::string>> pcUniforms;      // → push_constant
    std::vector<std::pair<std::string,std::string>> samplerUniforms; // → descriptor set
    std::vector<std::pair<std::string,std::string>> stageIns;
    std::vector<std::pair<std::string,std::string>> stageOuts;

    for (const auto& node : nodes) {
        if (auto* n = dynamic_cast<const UniformDeclExprAST*>(node.get())) {
            if (isSamplerOrImage(n->TypeName))
                samplerUniforms.push_back({n->TypeName, n->Name});
            else
                pcUniforms.push_back({n->TypeName, n->Name});
        } else if (auto* n = dynamic_cast<const UniformArrayDeclExprAST*>(node.get())) {
            pcUniforms.push_back({n->TypeName + "[" + std::to_string(n->Size) + "]", n->Name});
        } else if (auto* n = dynamic_cast<const StageVarDeclAST*>(node.get())) {
            if (n->isInput)  stageIns.push_back({n->TypeName, n->Name});
            else             stageOuts.push_back({n->TypeName, n->Name});
        }
    }

    // Sampler/image uniforms → layout(set=0, binding=N) (auto-assigned)
    int bindingIdx = 0;
    for (const auto& s : samplerUniforms)
        out += "layout(set = 0, binding = " + std::to_string(bindingIdx++) +
               ") uniform " + s.first + " " + s.second + ";\n";
    if (!samplerUniforms.empty()) out += "\n";

    // Scalar/vector uniforms → push_constant block + #define aliases.
    // The #define is placed AFTER the block so field names inside it are unaffected.
    if (!pcUniforms.empty()) {
        out += "layout(push_constant) uniform PC {\n";
        for (const auto& u : pcUniforms)
            out += "    " + u.first + " " + u.second + ";\n";
        out += "} pc;\n";
        for (const auto& u : pcUniforms)
            out += "#define " + u.second + " pc." + u.second + "\n";
        out += "\n";
    }

    // Stage in/out with explicit locations
    int inLoc = 0;
    for (const auto& v : stageIns)
        out += "layout(location = " + std::to_string(inLoc++) +
               ") in " + v.first + " " + v.second + ";\n";
    int outLoc = 0;
    for (const auto& v : stageOuts)
        out += "layout(location = " + std::to_string(outLoc++) +
               ") out " + v.first + " " + v.second + ";\n";
    if (!stageIns.empty() || !stageOuts.empty()) out += "\n";

    // Struct declarations
    for (const auto& node : nodes) {
        if (auto* n = dynamic_cast<const StructDeclExprAST*>(node.get())) {
            out += "struct " + n->Name + " {\n";
            for (const auto& f : n->Fields)
                out += "    " + f.first + " " + f.second + ";\n";
            out += "};\n\n";
        }
    }

    // Non-entry helper functions
    for (const auto& node : nodes) {
        auto* fn = dynamic_cast<const FunctionAST*>(node.get());
        if (!fn || fn->Attrs.isEntry) continue;
        out += fn->Proto->RetType + " " + fn->Proto->Name + "(";
        for (size_t i = 0; i < fn->Proto->Args.size(); ++i) {
            if (i > 0) out += ", ";
            out += fn->Proto->Args[i].first + " " + fn->Proto->Args[i].second;
        }
        out += ") ";
        if (auto* b = dynamic_cast<const BlockExprAST*>(fn->Body.get()))
            emitBlock(b, out, 0);
        out += "\n\n";
    }

    // Entry function → void main()
    for (const auto& node : nodes) {
        auto* fn = dynamic_cast<const FunctionAST*>(node.get());
        if (!fn || !fn->Attrs.isEntry) continue;
        out += "void main() ";
        if (auto* b = dynamic_cast<const BlockExprAST*>(fn->Body.get()))
            emitBlock(b, out, 0);
        out += "\n";
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char* outPath = (argc >= 2) ? argv[1] : "module.spv";
    // Initialize codegen state (shared globals used by the AST/parser machinery).
    InitializeModule();
    NamedValues.clear();

    getNextToken();
    auto nodes = ParseProgram();
    if (nodes.empty()) {
        std::cerr << "[irgen_spirv] Parse failed or empty program.\n";
        return 1;
    }

    // Detect shader stage from the entry function attribute
    ShaderStage stage = ShaderStage::Fragment;
    for (const auto& node : nodes) {
        if (auto* fn = dynamic_cast<const FunctionAST*>(node.get())) {
            if (fn->Attrs.isEntry && fn->Attrs.stage.has_value()) {
                stage = *fn->Attrs.stage;
                break;
            }
        }
    }

    // Generate GLSL source from the AST
    std::string glslSrc = generateGLSL(nodes);

    // Debug: print the generated GLSL when IRGEN_SPIRV_DEBUG is set
    if (getenv("IRGEN_SPIRV_DEBUG")) {
        std::cerr << "=== Generated GLSL ===\n" << glslSrc
                  << "=== End GLSL ===\n";
    }

    // Choose temp file extension and -S flag based on detected stage
    bool isVert = (stage == ShaderStage::Vertex);
    const char* tmpPath   = isVert ? "/tmp/_irgen_spirv_tmp.vert"
                                   : "/tmp/_irgen_spirv_tmp.frag";
    const char* stageFlag = isVert ? "vert" : "frag";
    {
        std::ofstream f(tmpPath);
        if (!f) {
            std::cerr << "[irgen_spirv] Cannot write temp file " << tmpPath << "\n";
            return 1;
        }
        f << glslSrc;
    }

    // Find glslangValidator
    static const char* candidates[] = {
        "/usr/bin/glslangValidator",
        "/usr/local/bin/glslangValidator",
        nullptr
    };
    const char* glslang = nullptr;
    for (int i = 0; candidates[i]; ++i) {
        if (::access(candidates[i], X_OK) == 0) { glslang = candidates[i]; break; }
    }
    if (!glslang) {
        ::unlink(tmpPath);
        std::cerr << "[irgen_spirv] glslangValidator not found.\n"
                  << "Install with:  sudo apt install glslang-tools\n";
        return 1;
    }

    // Compile GLSL → SPIR-V
    std::string cmd = std::string(glslang) +
        " -V --target-env vulkan1.0 -S " + stageFlag +
        " " + tmpPath + " -o " + outPath + " 2>&1";
    int rc = std::system(cmd.c_str());
    ::unlink(tmpPath);

    if (rc != 0) {
        std::cerr << "[irgen_spirv] glslangValidator failed (exit " << rc << ").\n";
        if (!getenv("IRGEN_SPIRV_DEBUG")) {
            std::cerr << "Tip: set IRGEN_SPIRV_DEBUG=1 to see the generated GLSL.\n";
        }
        return 1;
    }

    std::cout << "Wrote " << outPath << "\n";
    return 0;
}

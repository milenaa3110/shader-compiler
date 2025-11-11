#include "AST.h"
#include <iostream>

#include "codegen_state.h"
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

using namespace llvm;

// ----------------- Pomoćne rutine (samo u ovom translation unit-u) -----------------
namespace {

static llvm::Value* toI32(llvm::Value* v) {
    using namespace llvm;
    if (!v) return nullptr;
    auto* ty = v->getType();
    if (ty->isIntegerTy(32)) return v;
    if (ty->isIntegerTy())   return Builder->CreateIntCast(v, Type::getInt32Ty(*TheContext), /*isSigned*/true, "idxi32");
    if (ty->isFloatingPointTy()) return Builder->CreateFPToSI(v, Type::getInt32Ty(*TheContext), "idxi32");
    std::cerr << "Index must be int/float convertible to i32\n";
    return nullptr;
}

Type* resolveTypeByName(StringRef name) {
    if (name == "double") return Type::getDoubleTy(*TheContext);
    if (name == "float")  return Type::getFloatTy(*TheContext);
    if (name == "int")    return Type::getInt32Ty(*TheContext);
    if (name == "uint")   return Type::getInt32Ty(*TheContext);
    if (name == "bool")   return Type::getInt1Ty(*TheContext);

    if (name == "vec2")
        return FixedVectorType::get(Type::getFloatTy(*TheContext), 2);
    if (name == "vec3")
        return FixedVectorType::get(Type::getFloatTy(*TheContext), 3);
    if (name == "vec4")
        return FixedVectorType::get(Type::getFloatTy(*TheContext), 4);
    if (name == "mat2" || name == "mat2x2") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 2);
        return ArrayType::get(colTy, 2);
    }
    if (name == "mat3" || name == "mat3x3") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 3);
        return ArrayType::get(colTy, 3);
    }
    if (name == "mat4" || name == "mat4x4") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 4);
        return ArrayType::get(colTy, 4);
    }
    if (name == "mat2x3") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 3);
        return ArrayType::get(colTy, 2);
    }
    if (name == "mat2x4") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 4);
        return ArrayType::get(colTy, 2);
    }

    if (name == "mat3x2") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 2);
        return ArrayType::get(colTy, 3);
    }

    if (name == "mat3x4") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 4);
        return ArrayType::get(colTy, 3);
    }

    if (name == "mat4x2") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 2);
        return ArrayType::get(colTy, 4);
    }

    if (name == "mat4x3") {
        auto* colTy = FixedVectorType::get(Type::getFloatTy(*TheContext), 3);
        return ArrayType::get(colTy, 4);
    }

    return nullptr;
}

Value* castScalarTo(Value* v, Type* dst) {
    Type* src = v->getType();
    if (src == dst) return v;

    // --- SCALAR → BOOL (i1) specijalni slučajevi ---
    if (dst->isIntegerTy(1)) {
        if (src->isIntegerTy()) {
            // bilo koji int → bool : v != 0
            Value* zero = ConstantInt::get(src, 0);
            return Builder->CreateICmpNE(v, zero, "tobool");
        }
        if (src->isFloatTy() || src->isDoubleTy()) {
            // float/double → bool : v != 0.0 (UNE da NaN bude "true" kao "nonzero")
            Value* zero = src->isDoubleTy()
                        ? static_cast<Value*>(ConstantFP::get(Type::getDoubleTy(*TheContext), 0.0))
                        : static_cast<Value*>(ConstantFP::get(Type::getFloatTy(*TheContext),  0.0f));
            return Builder->CreateFCmpUNE(v, zero, "tobool");
        }
        // drugi slučajevi za i1 ne podržavamo u V1
        return nullptr;
    }

    // --- FLOAT/DOBULE ⇄ FLOAT/DOUBLE ---
    if (src->isDoubleTy() && dst->isFloatTy())  return Builder->CreateFPTrunc(v, dst, "truncf");
    if (src->isFloatTy()  && dst->isDoubleTy()) return Builder->CreateFPExt(v,   dst, "extd");

    // --- INT → FLOAT/DOUBLE ---
    if (src->isIntegerTy() && dst->isFloatTy())  return Builder->CreateSIToFP(v, dst, "sitofpf");
    if (src->isIntegerTy() && dst->isDoubleTy()) return Builder->CreateSIToFP(v, dst, "sitofpd");

    // --- FLOAT/DOUBLE → INT (≠ bool) ---
    if ((src->isFloatTy() || src->isDoubleTy()) && (dst->isIntegerTy() && !dst->isIntegerTy(1)))
        return Builder->CreateFPToSI(v, dst, "fptosi"); // i32/i64 je OK (ali i dalje UB van opsega cilja!)

    // --- INT ↔ INT (≠ bool) ---
    if (src->isIntegerTy() && dst->isIntegerTy()) {
        unsigned sw = src->getIntegerBitWidth();
        unsigned dw = dst->getIntegerBitWidth();
        if (sw == dw) return v;
        if (sw < dw)  return Builder->CreateSExt(v, dst, "sext");
        else          return Builder->CreateTrunc(v, dst, "trunci");
    }

    return nullptr;
}


// konstruisanje vektora iz N skalara (element type = float)
Value* buildVectorFromScalars(ArrayRef<Value*> scalars, FixedVectorType* vecTy) {
    Value* res = UndefValue::get(vecTy);
    for (unsigned i = 0; i < scalars.size(); ++i) {
        res = Builder->CreateInsertElement(res, scalars[i], Builder->getInt32(i), "ins");
    }
    return res;
}

// splat skalar -> vektor
Value* splatScalarToVector(Value* scalar, FixedVectorType* vecTy) {
    Value* res = UndefValue::get(vecTy);
    for (unsigned i = 0; i < vecTy->getNumElements(); ++i) {
        res = Builder->CreateInsertElement(res, scalar, Builder->getInt32(i), "splat");
    }
    return res;
}

    bool makeTypesMatch(Value*& L, Value*& R) {
    Type* LTy = L->getType();
    Type* RTy = R->getType();

    if (LTy == RTy) return true;

    // Vector + scalar -> splat scalar to vector
    if (LTy->isVectorTy() && !RTy->isVectorTy()) {
        auto* vecTy = cast<FixedVectorType>(LTy);
        Type* elemTy = vecTy->getElementType();
        Value* scalar = castScalarTo(R, elemTy);
        if (!scalar) return false;
        R = splatScalarToVector(scalar, vecTy);
        return true;
    }

    if (!LTy->isVectorTy() && RTy->isVectorTy()) {
        auto* vecTy = cast<FixedVectorType>(RTy);
        Type* elemTy = vecTy->getElementType();
        Value* scalar = castScalarTo(L, elemTy);
        if (!scalar) return false;
        L = splatScalarToVector(scalar, vecTy);
        return true;
    }

    // Scalar conversions
    if (!LTy->isVectorTy() && !RTy->isVectorTy()) {
        // Promote to higher precision type
        Type* targetTy = nullptr;

        // bool < int < float < double
        if (LTy->isDoubleTy() || RTy->isDoubleTy()) {
            targetTy = Type::getDoubleTy(*TheContext);
        } else if (LTy->isFloatTy() || RTy->isFloatTy()) {
            targetTy = Type::getFloatTy(*TheContext);
        } else if (LTy->isIntegerTy(32) || RTy->isIntegerTy(32)) {
            targetTy = Type::getInt32Ty(*TheContext);
        } else {
            targetTy = LTy; // keep as bool if both are bool
        }

        if (LTy != targetTy) {
            Value* casted = castScalarTo(L, targetTy);
            if (!casted) return false;
            L = casted;
        }

        if (RTy != targetTy) {
            Value* casted = castScalarTo(R, targetTy);
            if (!casted) return false;
            R = casted;
        }

        return true;
    }

    return false;
}


} // anon namespace

// ================= Number =================
Value* NumberExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val)); // double
}

// ================ Variable ================
Value* VariableExprAST::codegen() {
    auto it = NamedValues.find(Name);
    if (it == NamedValues.end()) {
        std::cerr << "Unknown variable: " << Name << "\n";
        return nullptr;
    }
    AllocaInst* A = it->second;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}

Value* UnaryExprAST::codegen() {
    Value* v = Operand->codegen();
    if (!v) {
        std::cerr << "Unary codegen: operand is null\n";
        return nullptr;
    }
    if (!v) return nullptr;

    if (Op == tok_minus || Op == '-') {
        if (v->getType()->isFloatTy() || v->getType()->isDoubleTy()) {
            return Builder->CreateFNeg(v, "negtmp");
        }
        if (v->getType()->isIntegerTy()) {
            return Builder->CreateNeg(v, "negtmp");
        }
        if (v->getType()->isVectorTy()) {
            return Builder->CreateFNeg(v, "negtmp");
        }
        std::cerr << "Unary minus not supported for this type\n";
        return nullptr;
    }
    return nullptr;
}

// ================ Binary (+) ===============
Value* BinaryExprAST::codegen() {
    Value* L = LHS->codegen();
    Value* R = RHS->codegen();
    if (!L || !R) return nullptr;

    if (!makeTypesMatch(L, R)) {
        std::cerr << "Type mismatch in binary op\n";
        return nullptr;
    }

    Type* opType = L->getType();

    // Aritmetički operatori
    if (Op == tok_plus || Op == '+') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFAdd(L, R, "addtmp");
        else if (opType->isIntegerTy())
            return Builder->CreateAdd(L, R, "addtmp");
    }

    if (Op == tok_minus || Op == '-') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFSub(L, R, "subtmp");
        else if (opType->isIntegerTy())
            return Builder->CreateSub(L, R, "subtmp");
    }

    if (Op == tok_multiply || Op == '*') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFMul(L, R, "multmp");
        else if (opType->isIntegerTy())
            return Builder->CreateMul(L, R, "multmp");
    }

    if (Op == tok_divide || Op == '/') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFDiv(L, R, "divtmp");
        else if (opType->isIntegerTy())
            return Builder->CreateSDiv(L, R, "divtmp");
    }

    // Poredenja - vraćaju bool (i1)
    if (Op == tok_less || Op == '<') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpOLT(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpSLT(L, R, "cmptmp");
    }

    if (Op == tok_less_equal) {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpOLE(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpSLE(L, R, "cmptmp");
    }

    if (Op == tok_greater || Op == '>') {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpOGT(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpSGT(L, R, "cmptmp");
    }

    if (Op == tok_greater_equal) {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpOGE(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpSGE(L, R, "cmptmp");
    }

    if (Op == tok_equal) {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpOEQ(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpEQ(L, R, "cmptmp");
    }

    if (Op == tok_not_equal) {
        if (opType->isFloatingPointTy() || opType->isVectorTy())
            return Builder->CreateFCmpONE(L, R, "cmptmp");
        else if (opType->isIntegerTy())
            return Builder->CreateICmpNE(L, R, "cmptmp");
    }

    std::cerr << "Unsupported binary operator\n";
    return nullptr;
}

static int charToIndex(char c) {
    switch (c) {
        case 'x': case 'r': case 's': return 0;
        case 'y': case 'g': case 't': return 1;
        case 'z': case 'b': case 'p': return 2;
        case 'w': case 'a': case 'q': return 3;
        default: return -1;
    }
}

Value* MemberAccessExprAST::codegen() {
    Value* obj = Object->codegen();
    if (!obj) return nullptr;

    Type* objTy = obj->getType();
    if (!objTy->isVectorTy()) {
        std::cerr << "Member access only supported on vectors\n";
        return nullptr;
    }

    auto* vecTy = cast<FixedVectorType>(objTy);
    unsigned numElements = vecTy->getNumElements();

    // Map member names to indices
    int index = charToIndex(Member[0]);

    if (index < 0 || index >= (int)numElements) {
        std::cerr << "Invalid vector component: " << Member << "\n";
        return nullptr;
    }

    return Builder->CreateExtractElement(obj, Builder->getInt32(index), "component");
}

Value* SwizzleExprAST::codegen() {
    Value* obj = Object->codegen();
    if (!obj) return nullptr;

    Type* objTy = obj->getType();
    if (!objTy->isVectorTy()) {
        std::cerr << "Swizzle only supported on vectors\n";
        return nullptr;
    }

    auto* vecTy = cast<FixedVectorType>(objTy);
    std::vector<int> indices;

    // Konvertuj swizzle string u indices
    for (char c : Swizzle) {
        int idx = charToIndex(c);
        if (idx < 0 || idx >= (int)vecTy->getNumElements()) {
            std::cerr << "Invalid swizzle component: " << c << "\n";
            return nullptr;
        }
        indices.push_back(idx);
    }

    Value* undefVec = UndefValue::get(obj->getType());
    return Builder->CreateShuffleVector(obj, undefVec, indices, "swizzle");
}

llvm::Value* SwizzleAssignmentExprAST::codegen() {
// Proveri da li je Object MatrixAccessExprAST (mat[i].xyz = ...)
    auto* matAccess = dynamic_cast<MatrixAccessExprAST*>(Object.get());
    if (matAccess) {
        // mat[i].xyz = value -> složeniji slučaj
        auto* lhsVar = dynamic_cast<VariableExprAST*>(matAccess->Object.get());
        if (!lhsVar) {
            std::cerr << "Matrix swizzle assignment LHS must be variable matrix\n";
            return nullptr;
        }

        auto it = NamedValues.find(lhsVar->Name);
        if (it == NamedValues.end()) {
            std::cerr << "Unknown matrix variable: " << lhsVar->Name << "\n";
            return nullptr;
        }
        llvm::AllocaInst* alloca = it->second;

        llvm::Type* matTy = alloca->getAllocatedType();
        if (!matTy->isArrayTy()) {
            std::cerr << "Expected matrix type for swizzle assignment\n";
            return nullptr;
        }

        auto* arrTy = llvm::cast<llvm::ArrayType>(matTy);
        auto* colTy = llvm::cast<llvm::FixedVectorType>(arrTy->getElementType());
        llvm::Type* elemTy = colTy->getElementType();
        unsigned rows = colTy->getNumElements();

        // Parsiraj swizzle
        std::vector<unsigned> indices;
        for (char c : Swizzle) {
            int idx = charToIndex(c);
            if (idx < 0 || (unsigned)idx >= rows) {
                std::cerr << "Invalid swizzle component for matrix column\n";
                return nullptr;
            }
            indices.push_back((unsigned)idx);
        }

        // Codegen kolona indeks
        llvm::Value* colIdx = matAccess->Index->codegen();
        if (!colIdx) return nullptr;
        colIdx = toI32(colIdx);
        if (!colIdx) return nullptr;

        // Codegen RHS
        llvm::Value* rhs = Init->codegen();
        if (!rhs) return nullptr;

        // Konvertuj RHS u komponente
        const unsigned K = (unsigned)indices.size();
        std::vector<llvm::Value*> comps;
        if (rhs->getType()->isVectorTy()) {
            auto* rVecTy = llvm::cast<llvm::FixedVectorType>(rhs->getType());
            if (rVecTy->getNumElements() != K) {
                std::cerr << "RHS vector size mismatch\n";
                return nullptr;
            }
            for (unsigned i = 0; i < K; ++i) {
                llvm::Value* e = Builder->CreateExtractElement(rhs, Builder->getInt32(i));
                if (e->getType() != elemTy) {
                    e = castScalarTo(e, elemTy);
                    if (!e) return nullptr;
                }
                comps.push_back(e);
            }
        } else {
            // Skalar splat
            llvm::Value* s = rhs;
            if (s->getType() != elemTy) {
                s = castScalarTo(s, elemTy);
                if (!s) return nullptr;
            }
            for (unsigned i = 0; i < K; ++i) comps.push_back(s);
        }

        // Učitaj matricu, modifikuj kolonu, vrati
        llvm::Value* curMat = Builder->CreateLoad(matTy, alloca);

        // Dinamički pristup koloni preko privremenog alloca
        llvm::AllocaInst* tmpA = CreateEntryBlockAlloca(
            Builder->GetInsertBlock()->getParent(), "tmp.mat", matTy);
        Builder->CreateStore(curMat, tmpA);

        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext), 0);
        llvm::Value* colPtr = Builder->CreateGEP(matTy, tmpA, {zero, colIdx}, "col.ptr");
        llvm::Value* colVal = Builder->CreateLoad(colTy, colPtr);

        // Modifikuj swizzle komponente
        llvm::Value* newCol = colVal;
        for (unsigned i = 0; i < K; ++i) {
            newCol = Builder->CreateInsertElement(newCol, comps[i],
                Builder->getInt32(indices[i]));
        }

        // Upiši nazad
        Builder->CreateStore(newCol, colPtr);
        llvm::Value* newMat = Builder->CreateLoad(matTy, tmpA);
        Builder->CreateStore(newMat, alloca);
        return newMat;
    }
    // LHS mora biti "assignable": ovde ograničimo na promenljivu (alloca)
    auto* lhsVar = dynamic_cast<VariableExprAST*>(Object.get());
    if (!lhsVar) {
        std::cerr << "Swizzle assignment LHS must be a variable (e.g., a.xy = ...)\n";
        return nullptr;
    }

    auto it = NamedValues.find(lhsVar->Name);
    if (it == NamedValues.end()) {
        std::cerr << "Unknown variable in swizzle assignment: " << lhsVar->Name << "\n";
        return nullptr;
    }
    llvm::AllocaInst* alloca = it->second;

    llvm::Type* objTy = alloca->getAllocatedType();
    if (!objTy->isVectorTy()) {
        std::cerr << "Swizzle assignment only supported on vector variables\n";
        return nullptr;
    }

    auto* vecTy  = llvm::cast<llvm::FixedVectorType>(objTy);
    llvm::Type* elemTy = vecTy->getElementType();
    unsigned N = vecTy->getNumElements();

    // Parsiraj swizzle u indekse
    std::vector<unsigned> indices;
    indices.reserve(Swizzle.size());
    std::vector<bool> touched(N, false);
    for (char c : Swizzle) {
        int idx = charToIndex(c);
        if (idx < 0 || (unsigned)idx >= N) {
            std::cerr << "Invalid swizzle component '" << c << "' for vec" << N << "\n";
            return nullptr;
        }
        // Zabranimo preklapanje (GLSL ponašanje)
        if (touched[(unsigned)idx]) {
            std::cerr << "Overlapping swizzle assignment (e.g., a.xx = ...) is not allowed\n";
            return nullptr;
        }
        touched[(unsigned)idx] = true;
        indices.push_back((unsigned)idx);
    }

    // Codegen RHS
    llvm::Value* rhs = Init->codegen();
    if (!rhs) return nullptr;

    // Iz RHS izvuci K komponenti (K = dužina swizzla), uz casting na elemTy
    const unsigned K = (unsigned)indices.size();
    std::vector<llvm::Value*> comps;
    comps.reserve(K);

    if (rhs->getType()->isVectorTy()) {
        auto* rVecTy = llvm::cast<llvm::FixedVectorType>(rhs->getType());
        unsigned rN = rVecTy->getNumElements();
        if (rN != K) {
            std::cerr << "RHS vector width (" << rN << ") must match swizzle length (" << K << ")\n";
            return nullptr;
        }
        llvm::Type* rElemTy = rVecTy->getElementType();
        for (unsigned i = 0; i < K; ++i) {
            llvm::Value* e = Builder->CreateExtractElement(rhs, Builder->getInt32(i), "rhs_comp");
            if (rElemTy != elemTy) {
                e = castScalarTo(e, elemTy);
                if (!e) return nullptr;
            }
            comps.push_back(e);
        }
    } else {
        // Skalar → splat na K komponenti
        llvm::Value* s = rhs;
        if (rhs->getType() != elemTy) {
            s = castScalarTo(rhs, elemTy);
            if (!s) return nullptr;
        }
        for (unsigned i = 0; i < K; ++i) comps.push_back(s);
    }

    // Učitaj trenutni vektor, zameni odabrane komponente, i upiši nazad
    llvm::Value* base = Builder->CreateLoad(objTy, alloca, (lhsVar->Name + ".old").c_str());
    llvm::Value* newVec = base;
    for (unsigned i = 0; i < K; ++i) {
        newVec = Builder->CreateInsertElement(newVec, comps[i], Builder->getInt32(indices[i]), "ins");
    }

    Builder->CreateStore(newVec, alloca);
    return newVec; // ili return comps.back() ako želiš vrednost izraza kao RHS poslednju komponentu
}


Value* BooleanExprAST::codegen() {
    return ConstantInt::get(Type::getInt1Ty(*TheContext), Val ? 1 : 0);
}

// ================ Call (type-constructors) ===============
Value* CallExprAST::codegen() {
    if (Callee.rfind("mat", 0) == 0) { // počinje sa "mat"
        unsigned cols = 0, rows = 0;
        {
            // parsiraj: matN ili matNxM (GLSL: C x R)
            size_t x = Callee.find('x');
            if (x == std::string::npos) {
                cols = rows = static_cast<unsigned>(Callee[3] - '0');
            } else {
                cols = static_cast<unsigned>(Callee[3] - '0');
                rows = static_cast<unsigned>(Callee[x + 1] - '0');
            }
        }

        auto* elemTy = Type::getFloatTy(*TheContext);
        auto* colTy  = FixedVectorType::get(elemTy, rows);
        auto* matTy  = ArrayType::get(colTy, cols);

        // () -> identitet
        if (Args.empty()) {
            Value* mat = ConstantAggregateZero::get(matTy);
            for (unsigned i = 0; i < std::min(cols, rows); ++i) {
                Value* col = Builder->CreateExtractValue(mat, i, "col");
                col = Builder->CreateInsertElement(col, ConstantFP::get(elemTy, 1.0), Builder->getInt32(i));
                mat = Builder->CreateInsertValue(mat, col, i);
            }
            return mat;
        }

        // (s) -> dijagonala=s, ostalo 0
        if (Args.size() == 1) {
            Value* s = Args[0]->codegen();
            if (!s) return nullptr;
            if (s->getType() != elemTy) {
                s = castScalarTo(s, elemTy);
                if (!s) return nullptr;
            }
            Value* mat = ConstantAggregateZero::get(matTy);
            for (unsigned i = 0; i < std::min(cols, rows); ++i) {
                Value* col = Builder->CreateExtractValue(mat, i, "col");
                col = Builder->CreateInsertElement(col, s, Builder->getInt32(i));
                mat = Builder->CreateInsertValue(mat, col, i);
            }
            return mat;
        }

        // (col0, col1, ... col{C-1}) gde je svaka vecR
        if (Args.size() == cols) {
            Value* mat = UndefValue::get(matTy);
            for (unsigned c = 0; c < cols; ++c) {
                Value* v = Args[c]->codegen();
                if (!v) return nullptr;
                if (!v->getType()->isVectorTy() || v->getType() != colTy) {
                    std::cerr << "Matrix constructor expects column " << c
                              << " to be vec" << rows << " of float\n";
                    return nullptr;
                }
                mat = Builder->CreateInsertValue(mat, v, c);
            }
            return mat;
        }

        // (C*R skalara) – kolona-major
        if (Args.size() == cols * rows) {
            Value* mat = UndefValue::get(matTy);
            unsigned k = 0;
            for (unsigned c = 0; c < cols; ++c) {
                Value* col = UndefValue::get(colTy);
                for (unsigned r = 0; r < rows; ++r, ++k) {
                    Value* a = Args[k]->codegen();
                    if (!a) return nullptr;
                    if (a->getType() != elemTy) {
                        a = castScalarTo(a, elemTy);
                        if (!a) return nullptr;
                    }
                    col = Builder->CreateInsertElement(col, a, Builder->getInt32(r));
                }
                mat = Builder->CreateInsertValue(mat, col, c);
            }
            return mat;
        }

        std::cerr << "Matrix constructor: unsupported argument pattern for "
                  << Callee << "\n";
        return nullptr;
    }

    if (Callee == "vec2" || Callee == "vec3" || Callee == "vec4") {
        unsigned N = (Callee == "vec2" ? 2u : Callee == "vec3" ? 3u : 4u);
        auto* elemTy = Type::getFloatTy(*TheContext);
        auto* vecTy  = FixedVectorType::get(elemTy, N);

        if (!(Args.size() == 1 || Args.size() == N)) {
            std::cerr << "Constructor " << Callee << " expects 1 or " << N << " arguments\n";
            return nullptr;
        }

        std::vector<Value*> vals;
        vals.reserve(Args.size());
        for (auto& a : Args) {
            Value* v = a->codegen();
            if (!v) return nullptr;

            if (v->getType()->isVectorTy()) {
                if (v->getType() != vecTy) {
                    std::cerr << "Type mismatch in " << Callee << " constructor\n";
                    return nullptr;
                }
                vals.push_back(v);
            } else {
                Value* casted = castScalarTo(v, elemTy); // double->float itd.
                if (!casted) {
                    std::cerr << "Cannot cast scalar arg to float for " << Callee << "\n";
                    return nullptr;
                }
                vals.push_back(casted);
            }
        }

        if (vals.size() == 1) {
            // splat: vecN(a)
            if (vals[0]->getType()->isVectorTy()) {
                if (vals[0]->getType() != vecTy) {
                    std::cerr << "Constructor splat expects exact " << N << "-wide float vector\n";
                    return nullptr;
                }
                return vals[0];
            }
            return splatScalarToVector(vals[0], vecTy);
        }

        // tačno N skalara
        return buildVectorFromScalars(vals, vecTy);
    }

    // (Opcionalno) skalarni konstruktori: float(x), double(x), ...
    if (Callee == "float" || Callee == "double" || Callee == "int" || Callee == "uint" || Callee == "bool") {
        if (Args.size() != 1) {
            std::cerr << "Constructor " << Callee << " expects exactly 1 argument\n";
            return nullptr;
        }
        Value* v = Args[0]->codegen();
        if (!v) return nullptr;
        Type* dst = resolveTypeByName(Callee);
        if (!dst) {
            std::cerr << "Unknown scalar constructor: " << Callee << "\n";
            return nullptr;
        }
        Value* cv = castScalarTo(v, dst);
        if (!cv) {
            std::cerr << "Cannot cast to " << Callee << "\n";
            return nullptr;
        }
        return cv;
    }

    std::cerr << "CallExpr not supported (only type constructors for now). Callee: " << Callee << "\n";
    return nullptr;
}

llvm::Value* IfExprAST::codegen() {
    llvm::Value* condVal = Condition->codegen();
    if (!condVal) return nullptr;

    llvm::Function* function = Builder->GetInsertBlock()->getParent();

    // Kreiraj osnovne blokove
    llvm::BasicBlock* thenBB  = llvm::BasicBlock::Create(*TheContext, "then", function);
    llvm::BasicBlock* elseBB  = ElseExpr ? llvm::BasicBlock::Create(*TheContext, "else") : nullptr;
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*TheContext, "ifend");

    // Grana prema odgovarajućem bloku
    Builder->CreateCondBr(condVal, thenBB, ElseExpr ? elseBB : mergeBB);

    // --- THEN blok ---
    Builder->SetInsertPoint(thenBB);
    if (!ThenExpr->codegen()) return nullptr;
    if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(mergeBB);

    // --- ELSE blok (ako postoji) ---
    if (ElseExpr) {
        function->insert(function->end(), elseBB);  // umesto getBasicBlockList()
        Builder->SetInsertPoint(elseBB);
        if (!ElseExpr->codegen()) return nullptr;
        if (!Builder->GetInsertBlock()->getTerminator())
            Builder->CreateBr(mergeBB);
    }

    // --- MERGE blok ---
    function->insert(function->end(), mergeBB);
    Builder->SetInsertPoint(mergeBB);

    return llvm::ConstantFP::get(llvm::Type::getFloatTy(*TheContext), 0.0);
}


llvm::Value* BlockExprAST::codegen() {
    llvm::Value* last = nullptr;
    for (auto& s : Statements) {
        last = s->codegen();
        if (!last) return nullptr;
    }
    if (!last)
        last = llvm::ConstantFP::get(llvm::Type::getFloatTy(*TheContext), 0.0f);
    return last;
}

// ================ Assignment ===============
llvm::Value* AssignmentExprAST::codegen() {
    if (!Builder || !Builder->GetInsertBlock() || !Builder->GetInsertBlock()->getParent()) {
        std::cerr << "Codegen error: IRBuilder has no valid insert point/function.\n";
        return nullptr;
    }
    Value* val = Init->codegen();
    if (!val) return nullptr;

    // Obična dodjela (bez tipa) - novo!
    if (VarType.empty()) {
        auto it = NamedValues.find(VarName);
        if (it == NamedValues.end()) {
            std::cerr << "Undefined variable in assignment: " << VarName << "\n";
            return nullptr;
        }
        AllocaInst* Alloca = it->second;

        // Type cast ako je potreban
        Type* targetType = Alloca->getAllocatedType();
        if (val->getType() != targetType) {
            if (targetType->isVectorTy() && !val->getType()->isVectorTy()) {
                auto* vecTy = cast<FixedVectorType>(targetType);
                Type* elemTy = vecTy->getElementType();
                Value* scalar = castScalarTo(val, elemTy);
                if (!scalar) {
                    std::cerr << "Cannot cast scalar to vector element type\n";
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            } else {
                Value* casted = castScalarTo(val, targetType);
                if (!casted) {
                    std::cerr << "Cannot cast value to target type\n";
                    return nullptr;
                }
                val = casted;
            }
        }

        Builder->CreateStore(val, Alloca);
        return val;
    }

    // POSTOJEĆI KOD za deklaraciju sa inicijalizacijom - ostaje isto!
    Type* Ty = resolveTypeByName(VarType);
    if (!Ty) {
        std::cerr << "Unknown type in assignment: " << VarType << "\n";
        return nullptr;
    }

    Function* F = Builder->GetInsertBlock()->getParent();
    AllocaInst* Alloca = nullptr;

    auto it = NamedValues.find(VarName);
    if (it == NamedValues.end()) {
        Alloca = CreateEntryBlockAlloca(F, VarName, Ty);
        NamedValues[VarName] = Alloca;
    } else {
        Alloca = it->second;
        if (Alloca->getAllocatedType() != Ty) {
            std::cerr << "Type mismatch for variable '" << VarName << "'\n";
            return nullptr;
        }
    }

    if (val->getType() != Ty) {
        if (Ty->isVectorTy()) {
            auto* vecTy = cast<FixedVectorType>(Ty);
            Type* elemTy = vecTy->getElementType();

            if (val->getType()->isVectorTy()) {
                std::cerr << "Vector type mismatch in assignment to " << VarName << "\n";
                return nullptr;
            } else {
                Value* scalar = castScalarTo(val, elemTy);
                if (!scalar) {
                    std::cerr << "Cannot cast scalar to vector element type\n";
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            }
        } else if (Ty->isDoubleTy() || Ty->isFloatTy() || Ty->isIntegerTy()) {
            Value* casted = castScalarTo(val, Ty);
            if (!casted) {
                std::cerr << "Cannot cast initializer to target scalar type\n";
                return nullptr;
            }
            val = casted;
        } else {
            std::cerr << "Unsupported target type in assignment\n";
            return nullptr;
        }
    }

    Builder->CreateStore(val, Alloca);
    return val;
}

llvm::Value* MatrixAssignmentExprAST::codegen() {
    using namespace llvm;

    // LHS mora biti promenljiva
    auto* lhsVar = dynamic_cast<VariableExprAST*>(Object.get());
    if (!lhsVar) {
        std::cerr << "Matrix assignment LHS must be a variable (e.g., M[i] = ... or M[i][j] = ...)\n";
        return nullptr;
    }

    auto it = NamedValues.find(lhsVar->Name);
    if (it == NamedValues.end()) {
        std::cerr << "Unknown variable in matrix assignment: " << lhsVar->Name << "\n";
        return nullptr;
    }
    AllocaInst* alloca = it->second;
    Type* matTy = alloca->getAllocatedType();

    if (!matTy->isArrayTy()) {
        std::cerr << "Matrix assignment requires matrix type (array of vectors)\n";
        return nullptr;
    }

    auto* arrTy = cast<ArrayType>(matTy);
    auto* colTy = cast<FixedVectorType>(arrTy->getElementType());
    Type* elemTy = colTy->getElementType(); // float
    unsigned rows = colTy->getNumElements();

    // i (kolona)
    Value* idxI = Index->codegen();
    if (!idxI) return nullptr;
    idxI = toI32(idxI);
    if (!idxI) return nullptr;

    // Učitaj trenutnu matricu
    Value* curMat = Builder->CreateLoad(matTy, alloca, (lhsVar->Name + ".old").c_str());

    if (!Index2) {
        // VARIJANTA: mat[i] = vecR
        Value* rhs = Init->codegen();
        if (!rhs) return nullptr;

        // RHS mora biti vecR ili skalar (splat)
        if (rhs->getType()->isVectorTy()) {
            if (rhs->getType() != colTy) {
                std::cerr << "RHS must be vec" << rows << " for matrix column assignment\n";
                return nullptr;
            }
        } else {
            // skalar → splat u vecR
            Value* s = rhs;
            if (s->getType() != elemTy) {
                s = castScalarTo(s, elemTy);
                if (!s) return nullptr;
            }
            // napravi vecR od skalara
            Value* tmp = UndefValue::get(colTy);
            for (unsigned r = 0; r < rows; ++r) {
                tmp = Builder->CreateInsertElement(tmp, s, Builder->getInt32(r), "colsplat");
            }
            rhs = tmp;
        }

        // Pošto su array-ovi vrednosni tip, InsertValue zahteva **konstantan** indeks.
        // Za dinamički i: prebacimo u memoriju pa GEP + store.
        // 1) napravimo privremeni alloca za matricu, upišemo curMat
        AllocaInst* tmpA = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(),
                                                  (lhsVar->Name + ".tmp").c_str(), matTy);
        Builder->CreateStore(curMat, tmpA);

        // 2) GEP do mat[i]
        Value* zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
        Value* colPtr = Builder->CreateGEP(matTy, tmpA, {zero, idxI}, "col.ptr");

        // 3) upiši kolonu
        Builder->CreateStore(rhs, colPtr);

        // 4) pročitaj nazad celu matricu i upiši u originalnu promenljivu
        Value* newMat = Builder->CreateLoad(matTy, tmpA, "mat.new");
        Builder->CreateStore(newMat, alloca);
        return newMat;
    } else {
        // VARIJANTA: mat[i][j] = scalar
        Value* idxJ = Index2->codegen();
        if (!idxJ) return nullptr;
        idxJ = toI32(idxJ);
        if (!idxJ) return nullptr;

        Value* rhs = Init->codegen();
        if (!rhs) return nullptr;

        if (rhs->getType() != elemTy) {
            rhs = castScalarTo(rhs, elemTy);
            if (!rhs) return nullptr;
        }

        // Slično: koristimo privremeni alloca, GEP do mat[i], load kolone, insert element, store nazad
        AllocaInst* tmpA = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(),
                                                  (lhsVar->Name + ".tmp").c_str(), matTy);
        Builder->CreateStore(curMat, tmpA);

        Value* zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
        Value* colPtr = Builder->CreateGEP(matTy, tmpA, {zero, idxI}, "col.ptr");
        Value* colVal = Builder->CreateLoad(colTy, colPtr, "col.val");

        Value* newCol = Builder->CreateInsertElement(colVal, rhs, idxJ, "col.ins");
        Builder->CreateStore(newCol, colPtr);

        Value* newMat = Builder->CreateLoad(matTy, tmpA, "mat.new");
        Builder->CreateStore(newMat, alloca);
        return newMat;
    }
}

llvm::Value* MatrixAccessExprAST::codegen() {
    using namespace llvm;

    Value* obj = Object->codegen();
    Value* i   = Index->codegen();
    if (!obj || !i) return nullptr;

    if (!obj->getType()->isArrayTy()) {
        std::cerr << "Matrix access only supported on matrix (array-of-vector) types\n";
        return nullptr;
    }

    i = toI32(i);
    if (!i) { std::cerr << "Matrix index i is not convertible to i32\n"; return nullptr; }

    // mat[i] → ExtractValue(mat, i) jer je mat ArrayType
    Value* col = Builder->CreateExtractValue(obj, {0u}, "col.tmp"); // placeholder da zadovoljimo tip
    (void)col; // samo da utišamo warning, stvarni Extract ima dinamički index:

    // Dinamički index nad ArrayType se radi preko GEP pa Load, ili preko CreateExtractValue sa konst. indeksom.
    // Pošto je i dinamički, koristimo GEP + Load:
    Value* zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    Value* colPtr = Builder->CreateInBoundsGEP(obj->getType(), obj, { zero, i }, "col.ptr");
    auto* ptrTy = llvm::cast<llvm::PointerType>(colPtr->getType());
    llvm::Type* elemTy = ptrTy->getNonOpaquePointerElementType();
    Value* colVal = Builder->CreateLoad(elemTy, colPtr, "col");

    if (!Index2) {
        // mat[i] → kolona (vecR)
        return colVal;
    }

    // mat[i][j] → element kolone
    Value* j = Index2->codegen();
    if (!j) return nullptr;
    j = toI32(j);
    if (!j) { std::cerr << "Matrix index j is not convertible to i32\n"; return nullptr; }

    return Builder->CreateExtractElement(colVal, j, "m.elem");
}


//===-- Cheerp/Writer.h - The Cheerp JavaScript generator -----------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2018 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#ifndef _CHEERP_WRITER_H
#define _CHEERP_WRITER_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Cheerp/AllocaMerging.h"
#include "llvm/Cheerp/GlobalDepsAnalyzer.h"
#include "llvm/Cheerp/LinearMemoryHelper.h"
#include "llvm/Cheerp/NameGenerator.h"
#include "llvm/Cheerp/PointerAnalyzer.h"
#include "llvm/Cheerp/Registerize.h"
#include "llvm/Cheerp/SourceMaps.h"
#include "llvm/Cheerp/Utility.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/FormattedStream.h"
#include <set>
#include <map>
#include <array>

struct Relooper;

struct Relooper;

class CheerpRenderInterface;

namespace cheerp
{

class NewLineHandler
{
};

/**
 * Black magic to conditionally enable indented output
 */
class ostream_proxy
{
public:
	ostream_proxy( llvm::raw_ostream & s, SourceMapGenerator* g, bool readableOutput = false ) :
		stream(s),
		sourceMapGenerator(g),
		readableOutput(readableOutput),
		newLine(true),
		indentLevel(0)
	{}

	friend ostream_proxy& operator<<( ostream_proxy & os, char c )
	{
		uint64_t begin = os.stream.tell();
		os.write_indent(c);
		uint64_t end = os.stream.tell();
		if(os.sourceMapGenerator)
			os.sourceMapGenerator->addLineOffset(end-begin);
		return os;
	}

	friend ostream_proxy& operator<<( ostream_proxy & os, llvm::StringRef s )
	{
		uint64_t begin = os.stream.tell();
		os.write_indent(s);
		uint64_t end = os.stream.tell();
		if(os.sourceMapGenerator)
			os.sourceMapGenerator->addLineOffset(end-begin);
		return os;
	}

	friend ostream_proxy& operator<<( ostream_proxy & os, const NewLineHandler& handler)
	{
		if(!os.readableOutput)
			return os;
		if(os.sourceMapGenerator)
			os.sourceMapGenerator->finishLine();
		os.stream << '\n';
		os.newLine = true;
		return os;
	}

	template<class T>
	friend typename std::enable_if<
		!std::is_convertible<T&&, llvm::StringRef>::value, // Use this only if T is not convertible to StringRef
		ostream_proxy&>::type operator<<( ostream_proxy & os, T && t )
	{
		uint64_t begin = os.stream.tell();
		if ( os.newLine && os.readableOutput )
			for ( int i = 0; i < os.indentLevel; i++ )
				os.stream << '\t';

		os.stream << std::forward<T>(t);
		os.newLine = false;
		uint64_t end = os.stream.tell();
		if(os.sourceMapGenerator)
			os.sourceMapGenerator->addLineOffset(end-begin);
		return os;
	}

	// Get the underlying stream, keep in mind that if you write to it then you need to manually synchronize the state with the
	// the 'syncRawStream' method to avoid breaking sourcemaps.
	llvm::raw_ostream & getRawStream() const
	{
		return stream;
	}

	void syncRawStream(uint64_t beginVal)
	{
		uint64_t end = stream.tell();
		if(sourceMapGenerator)
			sourceMapGenerator->addLineOffset(end-beginVal);
	}

private:

	// Return true if we are closing a curly bracket, need to unindent by 1.
	bool updateIndent( char c ) {
		if ( c == '{') indentLevel++;
		else if ( c == '}') {indentLevel--; return true; }
		return false;
	}

	// Return true if we are closing a curly, need to unindent by 1.
	bool updateIndent( llvm::StringRef s) {
		if (s.empty() )
			return false;
		bool ans = updateIndent(s.front());
		for (auto it = s.begin()+1; it != s.end(); ++it)
			updateIndent(*it);
		return ans;
	}

	template<class T>
	void write_indent(T && t)
	{
		int oldIndent = indentLevel;
		if (updateIndent( std::forward<T>(t) ) )
			oldIndent--;

		if ( newLine && readableOutput )
			for ( int i = 0; i < oldIndent; i++ )
				stream << '\t';

		stream << std::forward<T>(t);
		newLine = false;
	}

	llvm::raw_ostream & stream;
	SourceMapGenerator* sourceMapGenerator;
	bool readableOutput;
	bool newLine;
	int indentLevel;
};

class CheerpWriter
{
	friend class ::CheerpRenderInterface;
public:
	enum PARENT_PRIORITY { LOWEST = 0, TERNARY, LOGICAL_OR, LOGICAL_AND, BIT_OR, BIT_XOR, BIT_AND, COMPARISON, SHIFT, ADD_SUB, MUL_DIV, HIGHEST };
private:
	enum HEAP_TYPE {HEAP8=0, HEAP16, HEAP32, HEAPF32, HEAPF64};
	enum MODULE_TYPE { NONE = 0, CLOSURE, COMMONJS };

	llvm::Module& module;
	llvm::DataLayout targetData;
	const llvm::Function* currentFun;
	int currentPC;
	const llvm::BasicBlock* currentLandingPad;
	const PointerAnalyzer & PA;
	Registerize & registerize;

	GlobalDepsAnalyzer & globalDeps;
	const LinearMemoryHelper& linearHelper;
	const NameGenerator& namegen;
	const AllocaStoresExtractor& allocaStoresExtractor;
	TypeSupport types;
	std::set<const llvm::GlobalVariable*> compiledGVars;
	const std::array<const char*,5> typedArrayNames = {{"Uint8Array","Uint16Array","Int32Array","Float32Array","Float64Array"}};
	const std::array<llvm::StringRef,5> heapNames;

	// Stream to put the initialized asmjs memory into.
	llvm::raw_ostream* asmJSMem;
	// Name of the initialized asmjs memory file.
	const std::string& asmJSMemFile;

	// Support for source maps
	SourceMapGenerator* sourceMapGenerator;
	std::map<llvm::StringRef, llvm::DISubprogram> functionToDebugInfoMap;

	// Support for locals in deferred stacklets
	enum LOCAL_STATE { NOT_DONE, NAME_DONE, SECONDARY_NAME_DONE };
	struct LocalState
	{
		LocalState():state(NOT_DONE)
		{
		}
		llvm::StringRef name;
		llvm::StringRef secondaryName;
		LOCAL_STATE state;
		Registerize::REGISTER_KIND kind;
		bool isArg;
	};
	std::unordered_set<const llvm::Function*> deferredStacklets;
	const NewLineHandler NewLine;

	// Flag to signal if we should take advantage of native JavaScript math functions
	bool useNativeJavaScriptMath;
	// Flag to signal if we should take advantage of native 32-bit integer multiplication
	bool useMathImul;
	// Flag to signal if we should take advantage of native 32-bit float numbers
	bool useMathFround;
	// Enum to signal if we should create a module, and which kind
	MODULE_TYPE makeModule;
	// Flag to signal if we should add a credit comment line
	bool addCredits;
	// Flag to signal if we should add code that measures time until main is reached
	bool measureTimeToMain;
	// The asm.js module heap size
	uint32_t heapSize;
	// Flag to signal if we should add bounds-checking code
	bool checkBounds;
	// The name of the external wasm file, or empty if not present
	const std::string& wasmFile;
	// Flag to signal if we should generate typed arrays when element type is
	// double. Without this flag, normal arrays are used since they are
	// currently faster on v8.
	bool forceTypedArrays;
	// Flag to signal if we should use js variables instead of literals for globals addresses
	bool symbolicGlobalsAsmJS;
	// Flag to signal if we should emit readable or compressed output
	bool readableOutput;
	// Flag to signal if we should disable generation of Cheerp boilerplate around user code
	bool noBoilerplate;

	/**
	 * \addtogroup MemFunction methods to handle memcpy, memmove, mallocs and free (and alike)
	 *
	 * @{
	 */

	/**
	 * Compile memcpy and memmove
	 */
	void compileMemFunc(const llvm::Value* dest,
	                    const llvm::Value* srcOrResetVal,
	                    const llvm::Value* size);

	/**
	 * Copy baseSrc into baseDest
	 */
	void compileCopyElement(const llvm::Value* baseDest,
	                        const llvm::Value* baseSrc,
	                        llvm::Type* currentType);

	uint32_t compileArraySize(const DynamicAllocInfo& info, bool shouldPrint, bool isBytes = false);
	void compileAllocation(const DynamicAllocInfo& info);
	void compileFree(const llvm::Value* obj);

	/** @} */

	// COMPILE_EMPTY is returned if there is no need to add a ;\n to end the line
	enum COMPILE_INSTRUCTION_FEEDBACK { COMPILE_OK = 0, COMPILE_UNSUPPORTED, COMPILE_EMPTY };

	void handleBuiltinNamespace(const char* identifier, llvm::ImmutableCallSite callV);
	COMPILE_INSTRUCTION_FEEDBACK handleBuiltinCall(llvm::ImmutableCallSite callV, const llvm::Function* f);

	void compilePredicate(llvm::CmpInst::Predicate p);

	/**
	 * \addtogroup Pointers Methods to compile pointers
	 * @{
	 */

	/**
	 * Compile an == or != pointer comparison
	 */
	void compileEqualPointersComparison(const llvm::Value* lhs, const llvm::Value* rhs, llvm::CmpInst::Predicate p);

	/**
	 * Writes an access expression (i.e. something like .a3.a1[n].a4, etc.. ) using the given indices
	 */
	void compileAccessToElement(llvm::Type* tp, llvm::ArrayRef< const llvm::Value* > indices, bool compileLastWrapperArray);

	/**
	 * Write the offset part of a GEP as a literal or numerical offset
	 */
	void compileOffsetForGEP(llvm::Type* pointerOperandType, llvm::ArrayRef< const llvm::Value* > indices);

	/**
	 * Compile a COMPLETE_OBJECT.
	 * If the given value is a COMPLETE_OBJECT, just invoke compileOperand, otherwise do a promotion
	 */
	void compileCompleteObject(const llvm::Value*, const llvm::Value* offset = nullptr);

	/**
	 * Compile the pointer base.
	 */
	void compilePointerBase(const llvm::Value*, bool forEscapingPointer=false);

	/**
	 * Compile the pointer offset.
	 */
	void compilePointerOffset(const llvm::Value*, PARENT_PRIORITY parentPrio, bool forEscapingPointer=false);

	/**
	 * BYTE_LAYOUT_OFFSET_FULL: Compile the full offset in bytes till the element
	 * BYTE_LAYOUT_OFFSET_STOP_AT_ARRAY: Compile the offset in bytes till the array, if any, containing the element.
	 *                                   The offset into the array will be returned.
         * BYTE_LAYOUT_OFFSET_NO_PRINT: Like BYTE_LAYOUT_OFFSET_STOP_AT_ARRAY, but does not print any code.
	 */
	enum BYTE_LAYOUT_OFFSET_MODE { BYTE_LAYOUT_OFFSET_FULL = 0, BYTE_LAYOUT_OFFSET_STOP_AT_ARRAY, BYTE_LAYOUT_OFFSET_NO_PRINT };
	/**
	 * Compile the offset in bytes from the byte layout base found by recursively traversing BitCasts and GEPs.
	 * If a GEP from a byte layout pointer to an immutable type is contained in an ArrayType we want to construct the typed array
	 * starting from the array itself instead of from the value. This will make it possible to loop backward over the array.
	 */
	const llvm::Value* compileByteLayoutOffset(const llvm::Value* p, BYTE_LAYOUT_OFFSET_MODE offsetMode, PARENT_PRIORITY parentPrio);
	void compileByteLayoutAdd(const std::vector<std::pair<const llvm::Value*, size_t>>& dynPart, uint32_t constPart, PARENT_PRIORITY parentPrio);

	void compileRawPointer(const llvm::Value* p, PARENT_PRIORITY prio = PARENT_PRIORITY::LOWEST, bool forceGEP = false);

	/**
	 * Compile a pointer from a GEP expression, with the given pointer kind
	 */
	void compileGEP(const llvm::User* gepInst, POINTER_KIND kind);
	void compileGEPBase(const llvm::User* gep_inst, bool forEscapingPointer);
	void compileGEPOffset(const llvm::User* gep_inst, PARENT_PRIORITY parentPrio);

	/**
	 * Compile a pointer with the specified kind
	 */
	void compilePointerAs(const llvm::Value* p, POINTER_KIND kind)
	{
		assert(p->getType()->isPointerTy());
		assert(kind != SPLIT_REGULAR);
		POINTER_KIND valueKind = PA.getPointerKind(p);

		if(kind == COMPLETE_OBJECT)
		{
			compileCompleteObject(p);
		}
		else if (kind == RAW)
		{
			compileRawPointer(p);
		}
		else if (llvm::isa<llvm::ConstantPointerNull>(p))
		{
			stream << "nullObj";
		}
		else if (PA.getConstantOffsetForPointer(p) ||
			(valueKind == SPLIT_REGULAR || valueKind == SPLIT_BYTE_LAYOUT)
			valueKind == RAW)
		{
			stream << "{d:";
			compilePointerBase(p, !isByteLayout(kind));
			stream << ",o:";
			compilePointerOffset(p, LOWEST);
			stream << "}";
		}
		else
		{
			assert(valueKind == REGULAR || valueKind == BYTE_LAYOUT);
			compileOperand(p);
		}
	}

	/**
	 * Decide if `v` needs to be coerced to its integer type, based on the value of
	 * `coercionPrio`: If `coercionPrio` is `BIT_OR`,`BIT_AND`, or `SHIFT`,
	 * it means that the coercion has already been done, and can be skipped by
	 * the caller of this function.
	 */
	bool needsIntCoercion(Registerize::REGISTER_KIND kind, PARENT_PRIORITY coercionPrio)
	{
		if (kind == Registerize::INTEGER)
		{
			return (coercionPrio != BIT_OR && coercionPrio != BIT_AND && coercionPrio != SHIFT);
		}
		return false;
	}
	/**
	 * Return the next priority higher than `prio`.
	 * For binary operators in general the rhs must increment the priority
	 * if there are more operators with the same priority (e.g. FMul,FDiv,FRem)
	 */
	PARENT_PRIORITY nextPrio(PARENT_PRIORITY prio)
	{
		int new_prio = prio;
		return static_cast<PARENT_PRIORITY>(++new_prio);
	}

	/**
	 * Compile a (possibly dynamic) downcast
	 */
	void compileDowncast(llvm::ImmutableCallSite callV);
	/**
	 * Compile a cast to a virtual base
	 */
	void compileVirtualcast(llvm::ImmutableCallSite callV);

	/** @} */

	void compileConstantExpr(const llvm::ConstantExpr* ce);
	bool doesConstantDependOnUndefined(const llvm::Constant* C) const;
	void compileMethodArgs(llvm::User::const_op_iterator it, llvm::User::const_op_iterator itE, llvm::ImmutableCallSite, bool forceBoolean);
	COMPILE_INSTRUCTION_FEEDBACK compileTerminatorInstruction(const llvm::TerminatorInst& I);
	COMPILE_INSTRUCTION_FEEDBACK compileNotInlineableInstruction(const llvm::Instruction& I, PARENT_PRIORITY parentPrio);
	COMPILE_INSTRUCTION_FEEDBACK compileInlineableInstruction(const llvm::Instruction& I, PARENT_PRIORITY parentPrio);

	void compileSignedInteger(const llvm::Value* v, bool forComparison, PARENT_PRIORITY parentPrio);
	void compileUnsignedInteger(const llvm::Value* v, bool forAsmJSComparison, PARENT_PRIORITY parentPrio);

	void compileMethodLocal(llvm::StringRef name, Registerize::REGISTER_KIND kind, bool needsStacklet, bool isArg, bool stackletSync);
	void compileMethodLocals(const llvm::Function& F, std::set<uint32_t>& usedPCs, bool needsLabel, bool forceNoStacklet, bool stackletSync, bool hasLandingPad);
	void compileMethod(const llvm::Function& F);
	/**
	 * Helper structure for compiling globals
	 */
	struct GlobalSubExprInfo
	{
		POINTER_KIND kind;
		bool hasConstantOffset;
		GlobalSubExprInfo(POINTER_KIND k, bool h):kind(k),hasConstantOffset(h)
		{
		}
	};
	/**
	 * Helper method for compiling globals
	 */
	GlobalSubExprInfo compileGlobalSubExpr(const GlobalDepsAnalyzer::SubExprVec& subExpr);
	void compileGlobal(const llvm::GlobalVariable& G);
	void compileParamTypeAnnotationsAsmJS(const llvm::Function* F);
	void compileGlobalAsmJS(const llvm::GlobalVariable& G);
	void compileGlobalsInitAsmJS();
	void compileNullPtrs();
	void compileCreateClosure();
	void compileHandleVAArg();
	void compileBuiltins(bool asmjs);
	void compileAsmJSImports();
	void compileAsmJSExports();
	/**
	 * This method compiles an helper function for getting an ArrayBuffer from
	 * a file, usable from the browser and node
	 */
	void compileFetchBuffer();
	/**
	 * This method supports both ConstantArray and ConstantDataSequential
	 */
	void compileConstantArrayMembers(const llvm::Constant* C);

	/**
	 * Methods implemented in Types.cpp
	 */
	enum COMPILE_TYPE_STYLE { LITERAL_OBJ=0, THIS_OBJ };
	void compileTypedArrayType(llvm::Type* t);
	void compileSimpleType(llvm::Type* t, llvm::Value* init);
	// varName is used for a fake assignment to break literals into smaller units.
	// This is useful to avoid a huge penalty on V8 when creating large literals
	uint32_t compileComplexType(llvm::Type* t, COMPILE_TYPE_STYLE style, llvm::StringRef varName, uint32_t maxDepth, uint32_t totalLiteralProperties,
					const AllocaStoresExtractor::OffsetToValueMap* offsetToValueMap, uint32_t offset, uint32_t& usedValuesFromMap);
	void compileType(llvm::Type* t, COMPILE_TYPE_STYLE style, llvm::StringRef varName = llvm::StringRef(), const AllocaStoresExtractor::OffsetToValueMap* offsetToValueMap = nullptr);
	uint32_t compileClassTypeRecursive(const std::string& baseName, llvm::StructType* currentType, uint32_t baseCount);
	void compileClassType(llvm::StructType* T);
	void compileClassConstructor(llvm::StructType* T);
	void compileArrayClassType(llvm::Type* T);
	void compileArrayPointerType();
	bool needsUnsignedTruncation(std::unordered_set<const llvm::Value*> visited, const llvm::Value* v) const;
	bool needsUnsignedTruncation(const llvm::Value* v) const;

	/**
	 * Methods implemented in Opcodes.cpp
	 */
	void compileIntegerComparison(const llvm::Value* lhs, const llvm::Value* rhs, llvm::CmpInst::Predicate p, PARENT_PRIORITY parentPrio);
	void compilePtrToInt(const llvm::Value* v);
	void compileSubtraction(const llvm::Value* lhs, const llvm::Value* rhs, PARENT_PRIORITY parentPrio);
	void compileBitCast(const llvm::User* bc_inst, POINTER_KIND kind);
	void compileBitCastBase(const llvm::Value* op, llvm::Type* dstType, bool forEscapingPointer);
	void compileBitCastOffset(const llvm::Value* op, llvm::Type* dstType, PARENT_PRIORITY parentPrio);
	void compileSelect(const llvm::User* select, const llvm::Value* cond, const llvm::Value* lhs, const llvm::Value* rhs, PARENT_PRIORITY parentPrio);
	void compileAShr(const llvm::Value* lhs, const llvm::Value* rhs, PARENT_PRIORITY parentPrio);

	//JS interoperability support
	std::vector<llvm::StringRef> compileClassesExportedToJs();
	void addExportedFreeFunctions(std::vector<llvm::StringRef>& namesList, const llvm::NamedMDNode* namedNode);
	void compileInlineAsm(const llvm::CallInst& ci);

	static bool isByteLayout(POINTER_KIND k)
	{
		return k==BYTE_LAYOUT || k==SPLIT_BYTE_LAYOUT;
	}

	enum STACKLET_STATUS { NO_STACKLET = 0, STACKLET_NEEDED, STACKLET_NOT_NEEDED };
	STACKLET_STATUS needsStacklet(const llvm::Value* v) const;
	uint32_t getNextPC(const std::set<uint32_t>& usedPCs);

	struct JSBytesWriter: public LinearMemoryHelper::ByteListener
	{
		CheerpWriter& writer;
		bool first;
		JSBytesWriter(CheerpWriter& writer):writer(writer),first(true)
		{
		}
		void addByte(uint8_t b) override;
		uint32_t getObjectGlobalAddr(const llvm::Constant* C);
	};
	struct BinaryBytesWriter: public LinearMemoryHelper::ByteListener
	{
		ostream_proxy& stream;
		BinaryBytesWriter(ostream_proxy& stream):stream(stream)
		{
		}
		void addByte(uint8_t b) override {stream <<(char)b;};
	};

	struct AsmJSGepWriter: public LinearMemoryHelper::GepListener
	{
		CheerpWriter& writer;
		bool offset;
		bool use_imul;
		AsmJSGepWriter(CheerpWriter& writer, bool use_imul=true): writer(writer), offset(false), use_imul(use_imul)
		{
		}
		void addValue(const llvm::Value* v, uint32_t size) override;
		void addConst(int64_t v) override;
		bool isInlineable(const llvm::Value* p) override;
	};
	uint32_t lastObjectGlobalAddr = 1;
	std::unordered_map<const llvm::GlobalValue*, uint32_t> objectsGlobalAddrs;
	std::vector<const llvm::GlobalValue*> objectsGlobalOrder;
public:
	ostream_proxy stream;
	CheerpWriter(llvm::Module& m, llvm::raw_ostream& s, cheerp::PointerAnalyzer & PA,
			cheerp::Registerize & registerize,
			cheerp::GlobalDepsAnalyzer & gda,
			const cheerp::LinearMemoryHelper & linearHelper,
			cheerp::NameGenerator& namegen,
			cheerp::AllocaStoresExtractor& allocaStoresExtractor,
			llvm::raw_ostream* asmJSMem,
			const std::string& asmJSMemFile,
			SourceMapGenerator* sourceMapGenerator,
			bool readableOutput,
			llvm::StringRef makeModule,
			bool noRegisterize,
			bool useNativeJavaScriptMath,
			bool useMathImul,
			bool useMathFround,
			bool addCredits,
			bool measureTimeToMain,
			unsigned heapSize,
			bool checkBounds,
			bool compileGlobalsAddrAsmJS,
			const std::string& wasmFile,
			bool forceTypedArrays,
			bool NoBoilerplate):
		module(m),
		targetData(&m),
		currentFun(NULL),
		currentPC(0),
		currentLandingPad(NULL),
		PA(PA),
		registerize(registerize),
		globalDeps(gda),
		linearHelper(linearHelper),
		namegen(namegen),
		allocaStoresExtractor(allocaStoresExtractor),
		types(m),
		heapNames{{namegen.getBuiltinName(NameGenerator::Builtin::HEAP8), namegen.getBuiltinName(NameGenerator::Builtin::HEAP16),
			namegen.getBuiltinName(NameGenerator::Builtin::HEAP32), namegen.getBuiltinName(NameGenerator::Builtin::HEAPF32),
			namegen.getBuiltinName(NameGenerator::Builtin::HEAPF64)}},
		asmJSMem(asmJSMem),
		asmJSMemFile(asmJSMemFile),
		sourceMapGenerator(sourceMapGenerator),
		NewLine(),
		useNativeJavaScriptMath(useNativeJavaScriptMath),
		useMathImul(useMathImul),
		useMathFround(useMathFround),
		makeModule(makeModule==llvm::StringRef("closure")?
			MODULE_TYPE::CLOSURE : (makeModule==llvm::StringRef("commonjs")?
			MODULE_TYPE::COMMONJS : MODULE_TYPE::NONE)),
		addCredits(addCredits),
		measureTimeToMain(measureTimeToMain),
		heapSize(heapSize),
		checkBounds(checkBounds),
		wasmFile(wasmFile),
		forceTypedArrays(forceTypedArrays),
		symbolicGlobalsAsmJS(compileGlobalsAddrAsmJS),
		readableOutput(readableOutput),
		noBoilerplate(NoBoilerplate),
		stream(s, sourceMapGenerator, readableOutput)
	{
	}
	void makeJS();
	void compileBB(const llvm::BasicBlock& BB, const std::set<uint32_t>& usedPCs);
	void compileConstant(const llvm::Constant* c, PARENT_PRIORITY parentPrio = HIGHEST);
	void compileOperand(const llvm::Value* v, PARENT_PRIORITY parentPrio = HIGHEST, bool allowBooleanObjects = false);
	void compilePHIOfBlockFromOtherBlock(const llvm::BasicBlock* to, const llvm::BasicBlock* from, bool forceStackletSync);
	void compileOperandForIntegerPredicate(const llvm::Value* v, llvm::CmpInst::Predicate p, PARENT_PRIORITY parentPrio);

	// returns the amount fo shift required for accessing the corresponding heap
	int getHeapShiftForType(llvm::Type* et);
	int compileHeapForType(llvm::Type* et);
	void compileHeapAccess(const llvm::Value* p, llvm::Type* t = nullptr);
	/**
	 * Compile the function tables for the asm.js module
	 */
	void compileFunctionTablesAsmJS();
	/**
	 * Compile the declaration of the mathematical functions for the asm.js module
	 */
	void compileMathDeclAsmJS();
	/**
	 * Compile the memmove helper function for asm.js code
	 */
	void compileMemmoveHelperAsmJS();
	/**
	 * Compile the helper functions for exposing the asm.js stack pointer
	 */
	void compileStackPtrHelpersAsmJS();
	/**
	 * Compile a bound-checking statement on REGULAR or SPLIT_REGULAR pointer
	 */
	void compileCheckBounds(const llvm::Value* p);
	/**
	 * Compile a bound-checking function definition
	 */
	void compileCheckBoundsHelper();
	/**
	 * Compile a bound-checking statement for heap accesses in asm.js
	 */
	void compileCheckBoundsAsmJS(const llvm::Value* p, int alignMask);
	/**
	 * Compile a bound-checking function definition for asm.js heap
	 */
	void compileCheckBoundsAsmJSHelper();
	/**
	 * Compile a function to assure a GEP property access is defined
	 */
	void compileCheckDefined(const llvm::Value* p, bool needsOffset);
	/**
	 * Compile a function for checking if a reference is defined
	 */
	void compileCheckDefinedHelper();
	/**
	 * Compile a JS string while escaping special characters
	 */
	static void compileEscapedString(llvm::raw_ostream& stream, llvm::StringRef str, bool forJSON);
	/**
	 * Run relooper on a function, this code is here since it is also used by CheerpWastWriter
	 */
	static std::pair<Relooper*, const llvm::BasicBlock*> runRelooperOnFunction(const llvm::Function& F, const PointerAnalyzer& PA,
	                                       const Registerize& registerize, std::set<const llvm::BasicBlock*>* usedBlocks);
	static bool needsPointerKindConversion(const llvm::Instruction* phi, const llvm::Value* incoming,
						const PointerAnalyzer& PA, const Registerize& registerize);
	static bool needsPointerKindConversionForBlocks(const llvm::BasicBlock* to, const llvm::BasicBlock* from,
						const PointerAnalyzer& PA, const Registerize& registerize);
	uint32_t getObjectGlobalAddr(const llvm::GlobalValue* C);
};

}
#endif

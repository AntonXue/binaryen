/*
 * Copyright 2015 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_pass_h
#define wasm_pass_h

#include <functional>

#include "mixed_arena.h"
#include "support/utilities.h"
#include "wasm-traversal.h"
#include "wasm.h"

namespace wasm {

class Pass;

//
// Global registry of all passes in /passes/
//
struct PassRegistry {
  PassRegistry();

  static PassRegistry* get();

  typedef std::function<Pass*()> Creator;

  void registerPass(const char* name, const char* description, Creator create);
  Pass* createPass(std::string name);
  std::vector<std::string> getRegisteredNames();
  std::string getPassDescription(std::string name);

private:
  void registerPasses();

  struct PassInfo {
    std::string description;
    Creator create;
    PassInfo() = default;
    PassInfo(std::string description, Creator create)
      : description(description), create(create) {}
  };
  std::map<std::string, PassInfo> passInfos;
};

struct InliningOptions {
  // Function size at which we always inline.
  // Typically a size so small that after optimizations, the inlined code will
  // be smaller than the call instruction itself. 2 is a safe number because
  // there is no risk of things like
  //  (func $reverse (param $x i32) (param $y i32)
  //   (call $something (local.get $y) (local.get $x))
  //  )
  // in which case the reversing of the params means we'll possibly need
  // a block and a temp local. But that takes at least 3 nodes, and 2 < 3.
  // More generally, with 2 items we may have a local.get, but no way to
  // require it to be saved instead of directly consumed.
  Index alwaysInlineMaxSize = 2;
  // Function size which we inline when functions are lightweight (no loops
  // and calls) and we are doing aggressive optimisation for speed (-O3).
  // In particular it's nice that with this limit we can inline the clamp
  // functions (i32s-div, f64-to-int, etc.), that can affect perf.
  Index flexibleInlineMaxSize = 20;
  // Function size which we inline when there is only one caller.
  // FIXME: this should logically be higher than flexibleInlineMaxSize.
  Index oneCallerInlineMaxSize = 15;
};

struct PassOptions {
  // Run passes in debug mode, doing extra validation and timing checks.
  bool debug = false;
  // Whether to run the validator to check for errors.
  bool validate = true;
  // When validating validate globally and not just locally
  bool validateGlobally = false;
  // 0, 1, 2 correspond to -O0, -O1, -O2, etc.
  int optimizeLevel = 0;
  // 0, 1, 2 correspond to -O0, -Os, -Oz
  int shrinkLevel = 0;
  // Tweak thresholds for the Inlining pass.
  InliningOptions inlining;
  // Optimize assuming things like div by 0, bad load/store, will not trap.
  bool ignoreImplicitTraps = false;
  // Optimize assuming that the low 1K of memory is not valid memory for the
  // application to use. In that case, we can optimize load/store offsets in
  // many cases.
  bool lowMemoryUnused = false;
  enum { LowMemoryBound = 1024 };
  // Whether to try to preserve debug info through, which are special calls.
  bool debugInfo = false;
  // Arbitrary string arguments from the commandline, which we forward to
  // passes.
  std::map<std::string, std::string> arguments;

  void setDefaultOptimizationOptions() {
    // -Os is our default
    optimizeLevel = 2;
    shrinkLevel = 1;
  }

  static PassOptions getWithDefaultOptimizationOptions() {
    PassOptions ret;
    ret.setDefaultOptimizationOptions();
    return ret;
  }

  static PassOptions getWithoutOptimization() {
    return PassOptions(); // defaults are to not optimize
  }

  std::string getArgument(std::string key, std::string errorTextIfMissing) {
    if (arguments.count(key) == 0) {
      Fatal() << errorTextIfMissing;
    }
    return arguments[key];
  }
};

//
// Runs a set of passes, in order
//
struct PassRunner {
  Module* wasm;
  MixedArena* allocator;
  std::vector<Pass*> passes;
  PassOptions options;

  // PassRunner(Module* wasm) : wasm(wasm), allocator(&wasm->allocator) {}
  PassRunner(Module* wasm) : wasm(wasm), allocator(&wasm->allocator) {
    std::cout
      << "AX pass.h @ PassRunner("
      << static_cast<void*>(wasm) << ")"
      << std::endl;
  }
  // PassRunner(Module* wasm, PassOptions options)
  //   : wasm(wasm), allocator(&wasm->allocator), options(options) {}
  PassRunner(Module* wasm, PassOptions options)
    : wasm(wasm), allocator(&wasm->allocator), options(options) {
    std::cout
      << "Ax pass.h @ PassRunner("
      << std::hex << static_cast<void*>(wasm)
      << ", " << std::hex << static_cast<void*>(&options) << ")"
      << std::endl;
  }

  // no copying, we control |passes|
  PassRunner(const PassRunner&) = delete;
  PassRunner& operator=(const PassRunner&) = delete;

  void setDebug(bool debug) {
    options.debug = debug;
    // validate everything by default if debugging
    options.validateGlobally = debug;
  }
  void setDebugInfo(bool debugInfo) { options.debugInfo = debugInfo; }
  void setValidateGlobally(bool validate) {
    options.validateGlobally = validate;
  }

  void add(std::string passName) {
    std::cout
      << "AX pass.h @ PassRunner::add(" << passName << ")"
      << std::endl;
    auto pass = PassRegistry::get()->createPass(passName);
    if (!pass) {
      Fatal() << "Could not find pass: " << passName << "\n";
    }
    doAdd(pass);
  }

  template<class P> void add() { doAdd(new P()); }

  template<class P, class Arg> void add(Arg arg) { doAdd(new P(arg)); }

  // Adds the default set of optimization passes; this is
  // what -O does.
  void addDefaultOptimizationPasses();

  // Adds the default optimization passes that work on
  // individual functions.
  void addDefaultFunctionOptimizationPasses();

  // Adds the default optimization passes that work on
  // entire modules as a whole, and make sense to
  // run before function passes.
  void addDefaultGlobalOptimizationPrePasses();

  // Adds the default optimization passes that work on
  // entire modules as a whole, and make sense to
  // run after function passes.
  // This is run at the very end of the optimization
  // process - you can assume no other opts will be run
  // afterwards.
  void addDefaultGlobalOptimizationPostPasses();

  // Run the passes on the module
  void run();

  // Run the passes on a specific function
  void runOnFunction(Function* func);

  // Get the last pass that was already executed of a certain type.
  template<class P> P* getLast();

  ~PassRunner();

  // When running a pass runner within another pass runner, this
  // flag should be set. This influences how pass debugging works,
  // and may influence other things in the future too.
  void setIsNested(bool nested) { isNested = nested; }

  // BINARYEN_PASS_DEBUG is a convenient commandline way to log out the toplevel
  //                     passes, their times, and validate between each pass.
  //                     (we don't recurse pass debug into sub-passes, as it
  //                     doesn't help anyhow and also is bad for e.g. printing
  //                     which is a pass)
  // this method returns whether we are in passDebug mode, and which value:
  //  1: run pass by pass, validating in between
  //  2: also save the last pass, so it breakage happens we can print the last
  //  one 3: also dump out byn-* files for each pass
  static int getPassDebug();

protected:
  bool isNested = false;

private:
  void doAdd(Pass* pass);

  void runPass(Pass* pass);
  void runPassOnFunction(Pass* pass, Function* func);

  // After running a pass, handle any changes due to
  // how the pass is defined, such as clearing away any
  // temporary data structures that the pass declares it
  // invalidates.
  // If a function is passed, we operate just on that function;
  // otherwise, the whole module.
  void handleAfterEffects(Pass* pass, Function* func = nullptr);
};

//
// Core pass class
//
class Pass {
public:
  virtual ~Pass() = default;

  // Override this to perform preparation work before the pass runs.
  // This will be called before the pass is run on a module.
  virtual void prepareToRun(PassRunner* runner, Module* module) {}

  // Implement this with code to run the pass on the whole module
  virtual void run(PassRunner* runner, Module* module) { WASM_UNREACHABLE(); }

  // Implement this with code to run the pass on a single function, for
  // a function-parallel pass
  virtual void
  runOnFunction(PassRunner* runner, Module* module, Function* function) {
    WASM_UNREACHABLE();
  }

  // Function parallelism. By default, passes are not run in parallel, but you
  // can override this method to say that functions are parallelizable. This
  // should always be safe *unless* you do something in the pass that makes it
  // not thread-safe; in other words, the Module and Function objects and
  // so forth are set up so that Functions can be processed in parallel, so
  // if you do not ad global state that could be raced on, your pass could be
  // function-parallel.
  //
  // Function-parallel passes create an instance of the Walker class per
  // function. That means that you can't rely on Walker object properties to
  // persist across your functions, and you can't expect a new object to be
  // created for each function either (which could be very inefficient).
  //
  // It is valid for function-parallel passes to read (but not modify) global
  // module state, like globals or imports. However, reading other functions'
  // contents is invalid, as function-parallel tests can be run while still
  // adding functions to the module.
  virtual bool isFunctionParallel() { return false; }

  // This method is used to create instances per function for a
  // function-parallel pass. You may need to override this if you subclass a
  // Walker, as otherwise this will create the parent class.
  virtual Pass* create() { WASM_UNREACHABLE(); }

  // Whether this pass modifies the Binaryen IR in the module. This is true for
  // most passes, except for passes that have no side effects, or passes that
  // only modify other things than Binaryen IR (for example, the Stack IR
  // passes only modify that IR).
  // This property is important as if Binaryen IR is modified, we need to throw
  // out any Stack IR - it would need to be regenerated and optimized.
  virtual bool modifiesBinaryenIR() { return true; }

  std::string name;

protected:
  Pass() = default;
  Pass(Pass&) = default;
  Pass& operator=(const Pass&) = delete;
};

//
// Core pass class that uses AST walking. This class can be parameterized by
// different types of AST walkers.
//
template<typename WalkerType>
class WalkerPass : public Pass, public WalkerType {
  PassRunner* runner;

protected:
  typedef WalkerPass<WalkerType> super;

public:
  void run(PassRunner* runner, Module* module) override {
    // This is the thing that is called to start a Pass
    std::cout
      << "AX pass.h @ WalkerPass::run("
      << std::hex << static_cast<void*>(runner) << ", "
      << std::hex << static_cast<void*>(module) << ")"
      << std::endl;
    std::cout
      << "AX pass.h @ WalkerPass::run: this == "
      << static_cast<void*>(this)
      << std::endl;

    setPassRunner(runner);
    WalkerType::setModule(module);
    WalkerType::walkModule(module);
  }

  void
  runOnFunction(PassRunner* runner, Module* module, Function* func) override {
    std::cout
      << "AX pass.h @ WalkerPass::runOnFunction("
      << std::hex << static_cast<void*>(runner) << ", "
      << std::hex << static_cast<void*>(module) << ", "
      << std::hex << static_cast<void*>(func) << ")"
      << std::endl;
    std::cout
      << "AX pass.h @ WalkerPass::runOnFunction: this == "
      << static_cast<void*>(this)
      << std::endl;

    setPassRunner(runner);
    WalkerType::setModule(module);
    WalkerType::walkFunction(func);
  }

  PassRunner* getPassRunner() { return runner; }

  PassOptions& getPassOptions() { return runner->options; }

  void setPassRunner(PassRunner* runner_) { runner = runner_; }
};

} // namespace wasm

#endif // wasm_pass_h

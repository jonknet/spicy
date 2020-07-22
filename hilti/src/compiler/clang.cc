// Copyright (c) 2020 by the Zeek Project. See LICENSE for details.

// This file contains the implementation of Hilti’s JIT compiler for C++ code.
// The compiler takes C++ code generated by Hilti, compiles it down to LLVM IR
// using Clang, and emits a shared library using the system linker which
// finally gets loaded into the process.
//
// In order to compile C++ down to LLVM IR the compiler spins up Clang’s
// `CompilerInstance` for each source file given to it using standard clang
// command line arguments. Once the LLVM IR has been generated, we link it
// together into one Module using LLVM’s `Linker` class. Finally we use Clang
// to turn the linked module in a shared library on disk using the system
// linker. This shared library is loaded into the process with `::dlopen`.

#include "hilti/compiler/detail/clang.h"

#include <fstream>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// Leak checker API
#ifdef HILTI_HAVE_SANITIZER
#include <sanitizer/lsan_interface.h>
#endif

#include <hilti/rt/util.h>

#include <hilti/base/id-base.h>
#include <hilti/base/logger.h>
#include <hilti/base/result.h>
#include <hilti/base/timing.h>
#include <hilti/base/util.h>
#include <hilti/compiler/context.h>
#include <hilti/compiler/jit.h>

#include <llvm/ADT/None.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ilist_iterator.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/Option.h>
#include <llvm/Pass.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>

#include <clang/Basic/Version.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Options.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Lex/PreprocessorOptions.h>

namespace hilti::logging::debug {
inline const DebugStream Driver("driver");
} // namespace hilti::logging::debug

using namespace hilti;
using namespace hilti::detail;

#ifdef __APPLE__
#define SHARED_LIBRARY_EXTENSION ".dylib"
#else
#define SHARED_LIBRARY_EXTENSION ".so"
#endif

// PIMPL backend class
//
// Performance Notes(zeke):
//
// At the moment our methodology focuses more on making this work at all than
// making it work elegantly. Such is the nature of getting clang to work this
// way, but I think we could make this markedly more performant.
//
// Presently we compile every source file with a new CompilerInstance. This
// process of spinning up and then spinning down a CompilerInstance for every
// new bit of C++ code we see is very slow and dominates JIT performance.
// Luckily, clang's CompilerInstance has an incremental parsing mode where it
// appears that it can parse multiple C++ source files without trouble. There
// is an example of this in the clang repo and I think that that would be a
// great place to start exploring making this code faster.
struct ClangJIT::Implementation { // NOLINT
    Implementation(std::shared_ptr<Context> context);
    ~Implementation();

    /** See `ClangJit::compile()`. */
    bool compile(const std::string& file, std::optional<std::string> code);

    /** See `ClangJit::jit()`. */
    Result<Nothing> jit();

    /** See `ClangJit::retrieveLibrary()`. */
    std::optional<std::reference_wrapper<const Library>> retrieveLibrary() const;

    /** See `ClangJit::setDumpCode()`. */
    void setDumpCode() { dump_code = true; }

    /** Returns the compiler options in use. */
    auto options() const { return context->options(); }

private:
    /**
     * Links all previously compiled, individual LLVM modules into a joint
     * LLVM module.
     *
     * @return the linked, joint module, or null i an error occured
     */
    std::pair<std::unique_ptr<llvm::Module>, std::string> link();

    /**
     * Saves an LLVM bitcode file to disk.
     *
     * @param module bitcode to save
     * @param path path to file to write to
     */
    void saveBitcode(const llvm::Module& module, std::string path);

    /**
     * Compiles module to a native shared library.
     *
     * @param LLVM module to save
     * @return path to the created library
     */
    Result<Library> compileModule(llvm::Module&& module);

    /*
     * Creates an invocation for Clang's ``CompilerInstance`` class from its
     * arguments and ``-fsyntax-only`` using the provided driver. The
     * arguments should be in the same form as traditional clang arguments.
     *
     * @param args arguments to be passed to clang.
     * @param id the name of the current file / module being
     * compiled. Used for debugging purposes.
     * @param compiler_diags the diagnostics engine that the
     * CompilerInstance this is creating an invocation for will use.
     *
     * @return a CompilerInvocation ready for use with a CompilerInstance.
     */
    std::unique_ptr<clang::CompilerInvocation> createInvocationFromCommandLine(
        std::vector<std::string> args, const std::string& id, clang::DiagnosticsEngine& compiler_diags);

    /** Runs code optimization passes on a module.  */
    void optimizeModule(llvm::Module* m);

    /**
     * Adapts visilibity of global symbols in the final linked LLVM module.
     * This in particular internalizes symbols to the degree we can, and
     * renames selected exported symbols to have globally unique names.
     *
     * @param module LLVM module to operate on
     * @param symbols_to_uniquify symbols to rewrite to be globally unique,
     * even across runs; they will also be exported if they aren't already.
     *
     * @return a map of old-to-new symbols names for those in
     * ``symbols_to_uniquify``.
     */
    std::map<std::string, std::string> adaptSymbolVisibility(llvm::Module& module,
                                                             const std::set<std::string>& symbols_to_uniquify);

    // HILTI context to pull settings from.
    std::shared_ptr<Context> context;

    // The context for compilation and the JIT. Manages global state
    // over added modules.
    llvm::orc::ThreadSafeContext shared_context;

    // Adds definitions to our JIT so that it can properly link and
    // manage C++ code and does teardown when the JIT is done.
    llvm::orc::LocalCXXRuntimeOverrides runtime_overrides;

    // FIFO queue of Modules to be just in timed. The frontend class
    // will push Modules to this as it compiles C++ source files.
    std::queue<std::unique_ptr<llvm::Module>> module_queue;

    bool dump_code = false;

    std::optional<Library> shared_library;

    /*
     * Clones a llvm::Module to a new context. This effectively moves
     * control/management of the module's global state into the target
     * context.
     *
     * @param m the module to be cloned.
     * @param target the context that the module should be cloned
     * into.
     *
     * @return a new module who's global global data is managed by the
     * target context..
     */
    static std::unique_ptr<llvm::Module> cloneToContext(std::unique_ptr<llvm::Module> m,
                                                        llvm::LLVMContext& target); // NOLINT
};

ClangJIT::Implementation::Implementation(std::shared_ptr<Context> context)
    : context(context), shared_context(std::make_unique<llvm::LLVMContext>()) {
    // Initialize LLVM enviroment.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
}

ClangJIT::Implementation::~Implementation() { llvm::llvm_shutdown(); }

bool ClangJIT::Implementation::compile(const std::string& file, std::optional<std::string> code) {
    util::timing::Collector _("hilti/jit/clang/compile");

    // Build standard clang++ arguments.
    std::vector<std::string> args = {hilti::configuration().jit_clang_executable};

    if ( options().debug )
        args = util::concat(args, hilti::configuration().runtime_cxx_flags_debug);
    else
        args = util::concat(args, hilti::configuration().runtime_cxx_flags_release);

    for ( const auto& i : options().cxx_include_paths ) {
        args.emplace_back("-I");
        args.push_back(i);
    }

    // For debug output on compilation:
    // args.push_back("-v");
    // args.push_back("-###");

    if ( auto dir = hilti::configuration().jit_clang_resource_dir; ! dir.empty() ) {
        args.emplace_back("-resource-dir");
        args.emplace_back(dir.native());
    }

    args.push_back(file);

    // Reusing the driver across calls gives us trouble. In a perfect world,
    // we should be able to get around that by spinning up a compilerinstance
    // with incremental processing enabled and reusing that. For now though,
    // we are content with just creating a new compilerinstance and driver at
    // every call to compiler.
    //
    // If we would like to make compilation faster, I very much
    // beleive that this is a good place to start.
    HILTI_DEBUG(logging::debug::Jit, util::fmt("creating driver (%s)", util::join(args, " ")));

    clang::CompilerInstance clang_;

    clang_.createDiagnostics();
    if ( ! clang_.hasDiagnostics() ) {
        logger().error("jit: failed to create compilation diagnostics");
        return false;
    }

    auto ci = createInvocationFromCommandLine(args, file, clang_.getDiagnostics());

    if ( code ) {
        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(*code);
        ci->getPreprocessorOpts().addRemappedFile(file, buffer.release());
    }

    clang_.setInvocation(std::move(ci));

    // Force Clang to release its memory rather than reyling on process
    // termination to clean up.
    clang_.getFrontendOpts().DisableFree = false;

    // Aquire a lock on the context.
    auto lock = shared_context.getLock();
    auto action = std::make_unique<clang::EmitLLVMOnlyAction>();

    {
#ifdef HILTI_HAVE_SANITIZER
        // TODO(robin): Something in LLVM is leaking but I can't figure out
        // how to stop it. For now we just don't track anything allocated
        // during compilation in this block here.
        __lsan::ScopedDisabler llvm_leaks;
#endif

        if ( ! clang_.ExecuteAction(*action) ) {
            logger().error("jit: failed to execute compilation action.");
            return false;
        }
    }

    std::unique_ptr<llvm::Module> m = action->takeModule();
    if ( ! m ) {
        logger().error("jit: failed to generate LLVM IR for module");
        return false;
    }

    // Clone to shared context and move into queue. Not doing so causes seg
    // faults in hilti when it attempts certian lookups.
    module_queue.push(cloneToContext(std::move(m), *shared_context.getContext()));
    return true;
}

std::pair<std::unique_ptr<llvm::Module>, std::string> ClangJIT::Implementation::link() {
    util::timing::Collector _("hilti/jit/clang/link");

    // First, link all the modules in the queue together. Not doing this
    // causes illegal LLVM IR to be added to the JIT because we compile every
    // C++ source file independently.
    auto linked_module = std::make_unique<llvm::Module>("__LINKED__", *shared_context.getContext());
    llvm::Linker linker(*linked_module);

    while ( module_queue.size() ) {
        auto module = std::move(module_queue.front());
        module_queue.pop();

        if ( dump_code )
            saveBitcode(*module, util::fmt("dbg.%s.bc", module->getModuleIdentifier()));

        if ( linker.linkInModule(std::move(module), llvm::Linker::Flags::None) ) {
            logger().error("jit: error linking bitcode modules");
            return std::make_pair(nullptr, "");
        }
    }

    // Collect symbols that need to we need to rename to be globally
    // unique because (1) they must remain externally visible, and (2) we
    // may see them in more than one object file.
    std::set<std::string> symbols_to_uniquify = {"__linker__"};
    std::set<std::string> symbols_to_expose = {"hilti_main"};

    std::string one_ctor; // remembers one arbitrarty ctor for below

    for ( const auto& x : llvm::orc::getConstructors(*linked_module) ) {
        one_ctor = x.Func->getName().str();
        symbols_to_uniquify.insert(x.Func->getName().str());
    }

    for ( const auto& x : llvm::orc::getDestructors(*linked_module) )
        symbols_to_uniquify.insert(x.Func->getName().str());

    // Do the renaming. This returns a map mapping old names to the new
    // uniquified names.
    auto symbol_mapping = adaptSymbolVisibility(*linked_module, symbols_to_uniquify);
    auto unique_symbols = util::map_values(symbol_mapping);

    // Internalize all symbols that don't need to be externally visible. This
    // is adapted from code in llvm-link.cpp.
    auto must_preserve = [&](const llvm::GlobalValue& GV) {
        if ( GV.hasName() ) {
            if ( unique_symbols.find(std::string(GV.getName())) != unique_symbols.end() )
                return true; // don't internalize

            if ( symbols_to_expose.find(std::string(GV.getName())) != symbols_to_expose.end() )
                return true; // don't internalize
        }

        return ! GV.hasName();
    };

    llvm::InternalizePass(must_preserve).internalizeModule(*linked_module);

    if ( dump_code )
        saveBitcode(*linked_module, util::fmt("dbg.%s.bc", linked_module->getModuleIdentifier()));

    // We need to pick one externally visible symbol form the module, as
    // that's what we'll use to trigger materilization.
    auto all_exported_symbols = util::set_union(unique_symbols, symbols_to_expose);
    std::string linker_symbol = all_exported_symbols.size() ? *all_exported_symbols.begin() : std::string();

    return std::make_pair(std::move(linked_module), linker_symbol);
}

Result<Nothing> ClangJIT::Implementation::jit() {
    util::timing::Collector _("hilti/jit/clang/jit");

    if ( module_queue.size() ) {
        // Link all the pending modules together.
        auto [linked_module, linker_symbol] = link();
        if ( ! linked_module )
            return result::Error("jit: linking failed");

        if ( linker_symbol.empty() ) {
            HILTI_DEBUG(logging::debug::Jit, "skipping empty linked module");
            return Nothing();
        }

        std::string str;
        llvm::raw_string_ostream stream(str);
        if ( llvm::verifyModule(*linked_module, &stream) )
            return result::Error(util::fmt("jit: linked module failed verification (%s)", stream.str()));

        if ( options().optimize )
            optimizeModule(linked_module.get());

        auto library = compileModule(std::move(*linked_module));
        if ( ! library ) {
            logger().fatalError(util::fmt("could not create library: %s", library.error()));
        }

        shared_library.emplace(std::move(*library));

        if ( dump_code ) {
            constexpr char id[] = "__LINKED__";
            std::string path = std::string("dbg.") + id + SHARED_LIBRARY_EXTENSION;
            // Logging to driver because that's where all the other "saving to ..." messages go.
            HILTI_DEBUG(logging::debug::Driver, util::fmt("saving shared library for LLVM module %s to %s", id, path));
            if ( auto success = shared_library->save(path); ! success ) {
                return success;
            }
        }
    }

    return Nothing();
}

void ClangJIT::Implementation::optimizeModule(llvm::Module* m) {
    HILTI_DEBUG(logging::debug::Jit, util::fmt("optimizing module %s", m->getModuleIdentifier()));

    auto fpm = std::make_unique<llvm::legacy::FunctionPassManager>(m);
    // These are the reccomended optimization passes from
    // LLVM's build a JIT tutorial.
    fpm->add(llvm::createInstructionCombiningPass());
    fpm->add(llvm::createReassociatePass());
    fpm->add(llvm::createGVNPass());
    fpm->add(llvm::createCFGSimplificationPass());
    fpm->doInitialization();

    // Run the optimizations over all functions in the module being added to
    // the JIT.
    for ( auto& func : *m )
        fpm->run(func);
}

std::map<std::string, std::string> ClangJIT::Implementation::adaptSymbolVisibility(
    llvm::Module& module, const std::set<std::string>& symbols_to_uniquify) {
    std::map<std::string, std::string> new_symbols;

    auto process_global_value = [&](auto& v) {
        if ( v.isDeclaration() )
            return;

        if ( v.hasName() && symbols_to_uniquify.find(v.getName().str()) != symbols_to_uniquify.end() ) {
            // Make symbol (hopefully) unique by including its address into the name.
            std::string new_symbol = util::fmt("%s.%p", v.getName().str(), &module);
            new_symbols[std::string(v.getName())] = new_symbol;
            v.setName(new_symbol);

            // LLVM emits constructors as internal but we need to look them
            // up. Seems safe to adapt all uniquified symbols as they
            // presumably they should all be exported.
            if ( v.getLinkage() == llvm::GlobalValue::InternalLinkage )
                v.setLinkage(llvm::GlobalValue::ExternalLinkage);
        }
    };

    // The loops here follows https://stackoverflow.com/a/45323753
    for ( auto it = module.global_begin(); it != module.global_end(); ++it )
        process_global_value(*it);

    for ( auto it = module.alias_begin(); it != module.alias_end(); ++it )
        process_global_value(*it);

    for ( auto& F : module )
        process_global_value(F);

    return new_symbols;
}

std::optional<std::reference_wrapper<const Library>> ClangJIT::Implementation::retrieveLibrary() const {
    if ( ! shared_library )
        return std::nullopt;
    return *shared_library;
}

void ClangJIT::Implementation::saveBitcode(const llvm::Module& module, std::string path) {
    // Logging to driver because that's where all the other "saving to ..." messages go.
    HILTI_DEBUG(logging::debug::Driver,
                util::fmt("saving bitcode for LLVM module %s to %s", module.getModuleIdentifier(), path));
    std::ofstream out(path);
    llvm::raw_os_ostream lout(out);
    llvm::WriteBitcodeToFile(module, lout);
    lout.flush();
    out.flush();
}

Result<Library> ClangJIT::Implementation::compileModule(llvm::Module&& module) {
    util::timing::Collector _("hilti/jit/clang/save_library");

#ifdef HILTI_HAVE_SANITIZER
    // TODO(robin): Something in LLVM is leaking but I can't figure out
    // how to stop it. For now we just don't track anything allocated
    // during compilation in this block here.
    __lsan::ScopedDisabler llvm_leaks;
#endif

    // A helper function to remove temporary files on scope exit.
    auto cleanup_on_exit = [](const std::filesystem::path& tempfile) {
        return llvm::make_scope_exit([&]() {
            std::error_code ec;
            std::filesystem::remove(tempfile, ec);

            if ( ec ) {
                logger().error(util::fmt("cleanup of file %s failed: %s", tempfile, ec));
            }
        });
    };

    // Save object code to a temporary file.
    auto object_path = util::createTemporaryFile(std::string(module.getName()));

    if ( ! object_path )
        return object_path.error();

    auto c1 = cleanup_on_exit(*object_path);

    const auto& triple = module.getTargetTriple();
    HILTI_DEBUG(logging::debug::Jit, util::fmt("creating library with target triple '%s'", triple));

    std::string message;
    auto target = llvm::TargetRegistry::lookupTarget(triple, message);
    if ( ! target ) {
        return result::Error(util::fmt("could not look up target: %s", message));
    }

    auto machine = target->createTargetMachine(triple, "generic", "", llvm::TargetOptions(), llvm::Reloc::Model::PIC_);

    if ( context->options().optimize )
        machine->setOptLevel(llvm::CodeGenOpt::Aggressive);

    llvm::legacy::PassManager manager;

    std::error_code error;
    llvm::raw_fd_ostream file(object_path->native(), error);
    if ( error )
        return result::Error(util::fmt("could not open object file %s: %s", *object_path, error.message()));

#if LLVM_VERSION_MAJOR < 10
#define CGFT_OBJECTFILE llvm::TargetMachine::CGFT_ObjectFile
#else
#define CGFT_OBJECTFILE llvm::CGFT_ObjectFile
#endif
    if ( machine->addPassesToEmitFile(manager, file, nullptr, CGFT_OBJECTFILE) )
        return result::Error("adding passes failed");

    if ( ! manager.run(module) )
        return result::Error(util::fmt("object file %s could not be created", *object_path));

    file.close();

    // Use clang to link the object file into a shared library.
    clang::CompilerInstance clang_;
    clang_.createDiagnostics();
    if ( ! clang_.hasDiagnostics() ) {
        return result::Error("jit: failed to create compilation diagnostics");
    }

    auto library_path = util::createTemporaryFile(util::fmt("%s.hlto", module.getName().str()));
    if ( ! library_path )
        return library_path.error();

    auto c2 = cleanup_on_exit(*library_path);

    auto& diagnostics = clang_.getDiagnostics();

    std::vector<std::string> args = {hilti::configuration().jit_clang_executable,
                                     "-shared",
                                     "-Wl,-undefined",
                                     "-Wl,dynamic_lookup",
                                     *object_path,
                                     "-o",
                                     *library_path};

    auto driver = std::make_unique<clang::driver::Driver>(args[0], llvm::sys::getDefaultTargetTriple(), diagnostics);

    HILTI_DEBUG(logging::debug::Jit,
                util::fmt("compiling shared library %s with flags: %s", *library_path, util::join(args, " ")));

    auto cargs = util::transform(args, [](auto& s) -> const char* { return s.c_str(); });
    auto compilation = std::unique_ptr<clang::driver::Compilation>(driver->BuildCompilation(cargs));

    llvm::SmallString<1024> job_description;

    for ( const auto& job : compilation->getJobs() ) {
        llvm::raw_svector_ostream stream(job_description);
        job.Print(stream, "", false);
        HILTI_DEBUG(logging::debug::Jit, util::fmt("executing job for linking module: %s", job_description.c_str()));

        std::string error;
        bool failed = false;
        job.Execute({}, &error, &failed);

        if ( failed )
            return result::Error(util::fmt("could not create shared object: %s", error));
    }

    return Library(std::filesystem::absolute(*library_path));
}

std::unique_ptr<llvm::Module> ClangJIT::Implementation::cloneToContext(std::unique_ptr<llvm::Module> m,
                                                                       llvm::LLVMContext& target) {
    using namespace llvm;

    // BitcodeWriter requires that the Module has been materialized.
    if ( auto error = m->materializeAll() ) {
        logger().error("jit: failed to materialize module.");
        return nullptr;
    }

    SmallVector<char, 1> ClonedModuleBuffer;
    BitcodeWriter BCWriter(ClonedModuleBuffer);
    BCWriter.writeModule(*m);
    BCWriter.writeSymtab();
    BCWriter.writeStrtab();

    MemoryBufferRef ClonedModuleBufferRef(StringRef(ClonedModuleBuffer.data(), ClonedModuleBuffer.size()),
                                          "cloned module buffer");

    auto ClonedModule = llvm::cantFail(parseBitcodeFile(ClonedModuleBufferRef, target));

    ClonedModule->setModuleIdentifier(m->getName());
    return ClonedModule;
}

std::unique_ptr<clang::CompilerInvocation> ClangJIT::Implementation::createInvocationFromCommandLine(
    std::vector<std::string> args, const std::string& id, clang::DiagnosticsEngine& compiler_diags) {
    // FIXME: We shouldn't have to pass in the path info. Not doing so though
    // causes the compiler to fail to find headers.
    auto driver = std::make_unique<clang::driver::Driver>(args[0], llvm::sys::getDefaultTargetTriple(), compiler_diags);

    // Don't check that inputs exist, they may have been remapped.
    driver->setCheckInputsExist(false);

    // FIXME: Find a cleaner way to force the driver into restricted modes.
    args.emplace_back("-fsyntax-only");

    // Compile position-independent code so we can use the code in a shared library later.
    args.emplace_back("-fPIC");

    std::vector<const char*> cargs = util::transform(args, [](auto& s) -> const char* { return s.c_str(); });
    std::unique_ptr<clang::driver::Compilation> C(driver->BuildCompilation(cargs));
    if ( ! C )
        logger().fatalError("failed to build JIT compilation");

    // Print the cc1 options if -### was present.
    if ( C->getArgs().hasArg(clang::driver::options::OPT__HASH_HASH_HASH) )
        C->getJobs().Print(llvm::errs(), "\n", true);

    // We expect to get back exactly one command job, if we didn't something
    // failed.
    const clang::driver::JobList& jobs = C->getJobs();

    if ( jobs.empty() )
        logger().internalError("jit: no job in compilation");

    if ( jobs.size() > 1 )
        logger().internalError("jit: more than the expected 1 job in compilation");

    const clang::driver::Command& Cmd = llvm::cast<clang::driver::Command>(*jobs.begin());
    if ( std::string(Cmd.getCreator().getName()) != "clang" )
        logger().internalError("jit: unexpected job type in compilation");

    const llvm::opt::ArgStringList& CCArgs(Cmd.getArguments());

    HILTI_DEBUG(logging::debug::Jit, util::fmt("compiling module %s with \"%s\"", id, util::join(CCArgs, " ")));

    // clang::CompilerInvocation::Cr

    auto CI = std::make_unique<clang::CompilerInvocation>();

#if LLVM_VERSION_MAJOR < 10
    if ( ! clang::CompilerInvocation::CreateFromArgs(*CI, const_cast<const char**>(CCArgs.data()),
                                                     const_cast<const char**>(CCArgs.data()) + CCArgs.size(),
                                                     compiler_diags) )
#else
    if ( ! clang::CompilerInvocation::CreateFromArgs(*CI, CCArgs, compiler_diags) )
#endif
        logger().internalError("failed to create JIT compiler invocation");

    return CI;
}

ClangJIT::ClangJIT(std::shared_ptr<Context> context) : _impl(new ClangJIT::Implementation(context)) {}

ClangJIT::~ClangJIT() = default; // Needed here to allow ClangJIT to be forwarded declared.

std::string ClangJIT::compilerVersion() { return clang::getClangFullVersion(); }

bool ClangJIT::compile(const CxxCode& code) { return _impl->compile(util::fmt("%s.cc", code.id()), code.code()); }

bool ClangJIT::compile(const std::filesystem::path& p) { return _impl->compile(p, {}); }

Result<Nothing> ClangJIT::jit() { return _impl->jit(); }

std::optional<std::reference_wrapper<const Library>> ClangJIT::retrieveLibrary() const {
    return _impl->retrieveLibrary();
}

void ClangJIT::setDumpCode() { _impl->setDumpCode(); }

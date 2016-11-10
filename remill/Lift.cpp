/* Copyright 2015 Peter Goodman (peter@trailofbits.com), all rights reserved. */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/AssemblyWriter.h"
#include "remill/BC/Translator.h"
#include "remill/BC/Util.h"
#include "remill/CFG/CFG.h"

#ifndef REMILL_OS
# if defined(__APPLE__)
#   define REMILL_OS "mac"
# elif defined(__linux__)
#   define REMILL_OS "linux"
# endif
#endif

#ifndef BUILD_SEMANTICS_DIR
# error "Macro `BUILD_SEMANTICS_DIR` must be defined."
# define BUILD_SEMANTICS_DIR
#endif  // BUILD_SEMANTICS_DIR

#ifndef INSTALL_SEMANTICS_DIR
# error "Macro `INSTALL_SEMANTICS_DIR` must be defined."
# define INSTALL_SEMANTICS_DIR
#endif  // INSTALL_SEMANTICS_DIR


// TODO(pag): Support separate source and target architectures?
DEFINE_string(arch_in, "", "Architecture of the code being translated. "
                           "Valid architectures: x86, amd64 (with or without "
                           "`_avx` or `_avx512` appended).");

DEFINE_string(arch_out, "", "Architecture of the target architecture on "
                            "which the translated code will run. "
                            "Valid architectures: x86, amd64 (with or without "
                            "`_avx` or `_avx512` appended).");

DEFINE_string(os_in, REMILL_OS, "Source OS. Valid OSes: linux, mac.");
DEFINE_string(os_out, REMILL_OS, "Target OS. Valid OSes: linux, mac.");

DEFINE_string(cfg, "", "Path to the CFG file containing code to lift.");

DEFINE_string(bc_in, "", "Input bitcode file into which code will "
                         "be lifted. This should either be a semantics file "
                         "associated with `--arch_in`, or it should be "
                         "a bitcode file produced by `remill-lift`. Chaining "
                         "bitcode files produces by `remill-lift` can be "
                         "used to iteratively link in libraries to lifted "
                         "code.");

DEFINE_string(bc_out, "", "Output bitcode file name.");

DEFINE_string(asm_out, "", "Output disassembly file name. This is produced "
                           "by the translator and contains disassembled "
                           "instructions. Debug information references this "
                           "file.");

namespace {

static const char *gSearchPaths[] = {
    // Derived from the build.
    BUILD_SEMANTICS_DIR "\0",
    INSTALL_SEMANTICS_DIR "\0",

    // Linux.
    "/usr/local/share/remill/semantics",
    "/usr/share/remill/semantics",

    // Other?
    "/opt/local/share/remill/semantics",
    "/opt/share/remill/semantics",
    "/opt/remill/semantics",

    // FreeBSD.
    "/usr/share/compat/linux/remill/semantics",
    "/usr/local/share/compat/linux/remill/semantics",
    "/compat/linux/usr/share/remill/semantics",
    "/compat/linux/usr/local/share/remill/semantics",
};

static bool CheckPath(const std::string &path) {
  return !path.empty() && !access(path.c_str(), F_OK);
}

static std::string InputBCPath(void) {
  if (!FLAGS_bc_in.empty()) {
    return FLAGS_bc_in;
  }

  for (auto path : gSearchPaths) {
    std::stringstream ss;
    ss << path << "/" << FLAGS_arch_in << ".bc";
    auto sem_path = ss.str();
    if (CheckPath(sem_path)) {
      return sem_path;
    }
  }

  LOG(FATAL)
      << "Cannot find path to " << FLAGS_arch_in << " semantics bitcode file.";
}

}  // namespace

int main(int argc, char *argv[]) {
  std::stringstream ss;
  ss << std::endl << std::endl
     << "  " << argv[0] << " \\" << std::endl
     << "    [--bc_in INPUT_BC_FILE] \\" << std::endl
     << "    --bc_out OUTPUT_BC_FILE \\" << std::endl
     << "    --arch_in SOURCE_ARCH_NAME \\" << std::endl
     << "    [--arch_out TARGET_ARCH_NAME] \\" << std::endl
     << "    --os_in SOURCE_OS_NAME \\" << std::endl
     << "    [--os_our TARGET_OS_NAME] \\" << std::endl
     << "    --cfg CFG_FILE"
     << std::endl;

  google::SetUsageMessage(ss.str());
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // GFlags will have removed everything that it recognized from argc/argv.
  llvm::cl::ParseCommandLineOptions(argc, argv, "Remill: Lift CFG to LLVM");

  if (FLAGS_os_in.empty()) {
    std::cerr
        << "Need to specify a source operating system with --os_in."
        << std::endl;
    return EXIT_FAILURE;
  }

  if (FLAGS_os_out.empty()) {
    FLAGS_os_out = FLAGS_os_in;
    std::cerr
        << "Need to specify a target operating system with --os_out."
        << std::endl;
    return EXIT_FAILURE;
  }

  if (FLAGS_arch_in.empty()) {
    std::cerr
        << "Need to specify a source architecture with --arch_in."
        << std::endl;
    return EXIT_FAILURE;
  }

  if (FLAGS_arch_out.empty()) {
    FLAGS_arch_out = FLAGS_arch_in;
  }

  if (FLAGS_cfg.empty()) {
    std::cerr
        << "Must specify CFG file with --cfg."
        << std::endl;
    return EXIT_FAILURE;
  }

  if (FLAGS_bc_out.empty()) {
    std::cerr
        << "Please specify an output bitcode file with --bc_out."
        << std::endl;
    return EXIT_FAILURE;
  }

  auto source_os = remill::GetOSName(FLAGS_os_in);
  auto target_os = remill::GetOSName(FLAGS_os_out);

  auto source_arch = remill::Arch::Create(source_os, FLAGS_arch_in);
  auto target_arch = remill::Arch::Create(target_os, FLAGS_arch_out);

  if (!CheckPath(FLAGS_cfg)) {
    std::cerr
        << "Must specify valid path for `--cfg`. CFG file " << FLAGS_cfg
        << " cannot be opened."
        << std::endl;
    return EXIT_FAILURE;
  }

  FLAGS_bc_in = InputBCPath();
  if (!CheckPath(FLAGS_bc_in)) {
    std::cerr
        << "Must specify valid path for `--bc_in`. Bitcode file "
        << FLAGS_bc_in << " cannot be opened."
        << std::endl;
    return EXIT_FAILURE;
  }


  auto context = new llvm::LLVMContext;
  auto module = remill::LoadModuleFromFile(context, FLAGS_bc_in);
  target_arch->PrepareModule(module);

  remill::AssemblyWriter *asm_writer = nullptr;
  if (!FLAGS_asm_out.empty()) {
    asm_writer = new remill::AssemblyWriter(module, FLAGS_asm_out);
  }

  auto translator = new remill::Translator(source_arch, module, asm_writer);
  auto cfg = remill::ReadCFG(FLAGS_cfg);
  translator->LiftCFG(cfg);
  delete cfg;
  delete translator;
  delete source_arch;
  delete target_arch;
  if (asm_writer) {
    delete asm_writer;
    asm_writer = nullptr;
  }

  cfg = nullptr;
  translator = nullptr;
  source_arch = nullptr;
  target_arch = nullptr;

  remill::StoreModuleToFile(module, FLAGS_bc_out);
  delete module;
  delete context;

  module = nullptr;
  context = nullptr;

  google::ShutdownGoogleLogging();
  return EXIT_SUCCESS;
}

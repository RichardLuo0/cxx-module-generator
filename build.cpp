#ifdef _WIN32
#define POSTFIX ".exe"
#else
#define POSTFIX ""
#endif

import std;
import makeDotCpp;
import makeDotCpp.project;
import makeDotCpp.compiler.Clang;
import makeDotCpp.fileProvider.Glob;
import makeDotCpp.builder;

#include "project.json.hpp"

using namespace makeDotCpp;

int main(int argc, const char **argv) {
  std::deque<std::shared_ptr<Export>> packages;
  populatePackages(packages);

  Project::OptionParser op;
  op.parse(argc, argv);

  auto compiler = std::make_shared<Clang>();
  compiler->addOption("-std=c++20 -O3 -Wall")
      .addLinkOption("-lclang-cpp")
      .addLinkOption("-lLLVM-18")
      .addLinkOption("-Wl,--stack=4194304");

  ExeBuilder builder("cmg");
  builder.setCompiler(compiler)
      .addSrc(Glob("src/**/*.cppm"))
      .addSrc("src/main.cpp");

  for (auto &package : packages) {
    builder.dependOn(package);
  }

  Project()
      .setName("cxx-module-generator")
      .setBuild([&](const Context &ctx) {
        auto future = builder.build(ctx);
        future.get();
        std::cout << "\033[0;32mDone\033[0m" << std::endl;
      })
      .setInstall([](const Context &) {})
      .run(op);
}

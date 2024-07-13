import std;
import makeDotCpp;
import makeDotCpp.project;
import makeDotCpp.project.api;
import makeDotCpp.compiler.Clang;
import makeDotCpp.fileProvider.Glob;
import makeDotCpp.builder;

using namespace makeDotCpp;
using namespace api;

extern "C" int build(const ProjectContext &ctx) {
  auto compiler = std::make_shared<Clang>();
  compiler->addOption("-std=c++20 -O3 -Wall")
      .addLinkOption("-lclang-cpp")
      .addLinkOption("-lLLVM-18")
      .addLinkOption("-Wl,--stack=4194304");

  ExeBuilder builder("cmg");
  builder.addSrc(Glob("src/**/*.cppm")).addSrc("src/main.cpp");

  for (auto &package : ctx.packageExports | std::views::values) {
    builder.dependOn(package);
  }

  Project()
      .setName("cxx-module-generator")
      .setCompiler(compiler)
      .setBuild([&](const Context &ctx) {
        auto future = builder.build(ctx);
        future.get();
        std::cout << "\033[0;32mDone\033[0m" << std::endl;
      })
      .setInstall([](const Context &) {})
      .run(ctx.argc, ctx.argv);
  return 0;
}

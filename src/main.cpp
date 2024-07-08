#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace clang;
using namespace tooling;
namespace fs = std::filesystem;

static llvm::cl::OptionCategory toolCategory("cxx-module-generator");
static llvm::cl::opt<std::string> moduleName("name",
                                             llvm::cl::desc("Module name"),
                                             llvm::cl::cat(toolCategory));
static llvm::cl::opt<std::string> output("o",
                                         llvm::cl::desc("Specify output path"),
                                         llvm::cl::cat(toolCategory));
static llvm::cl::opt<std::string> nsFilter(
    "namespace", llvm::cl::desc("Filter symbol by namespace"),
    llvm::cl::cat(toolCategory));

class ModuleWrapper {
 private:
  std::string content;

 public:
  ModuleWrapper(const fs::path &originalFile) {
    content += "module;\n#include \"" + originalFile.generic_string() + "\"\n";
    content += "export module " +
               (!moduleName.empty() ? moduleName.getValue()
                                    : originalFile.stem().string()) +
               ";\n";
  }

  void openNamespace(NamespaceDecl *decl) {
    content += "namespace " + decl->getNameAsString() + " {\n";
  }

  void closeNamespace(NamespaceDecl *) { content += "}\n"; }

  void addSymbol(NamedDecl *decl) {
    if (decl != nullptr && decl->isFirstDecl())
      content += "export using " + decl->getQualifiedNameAsString() + ";\n";
  }

  const std::string &getContent() const { return content; }
};

class FindAllSymbols : public RecursiveASTVisitor<FindAllSymbols> {
 private:
  using Base = RecursiveASTVisitor<FindAllSymbols>;

  ModuleWrapper &wrapper;

 public:
  FindAllSymbols(ModuleWrapper &wrapper) : wrapper(wrapper) {}

  bool TraverseNamespaceDecl(NamespaceDecl *decl) {
    if (nsFilter.find(decl->getName()) != std::string::npos) {
      wrapper.openNamespace(decl);
      Base::TraverseNamespaceDecl(decl);
      wrapper.closeNamespace(decl);
    }
    return true;
  }

  bool TraverseDecl(Decl *decl) {
    switch (decl->getKind()) {
      case Decl::TranslationUnit:
        return TraverseTranslationUnitDecl(
            static_cast<TranslationUnitDecl *>(decl));
      case Decl::Namespace:
        return TraverseNamespaceDecl(static_cast<NamespaceDecl *>(decl));
      default:
        break;
    }
    switch (decl->getKind()) {
#define ABSTRACT_DECL(DECL)
#define DECL(CLASS, BASE)                                           \
  case Decl::CLASS:                                                 \
    if (!WalkUpFrom##CLASS##Decl(static_cast<CLASS##Decl *>(decl))) \
      return false;                                                 \
    break;
#include "clang/AST/DeclNodes.inc"
    }
    return true;
  }

#define VISIT_DECL(TYPE)                                                      \
  bool Visit##TYPE##Decl(TYPE##Decl *decl) {                                  \
    if (!decl->isImplicit() &&                                                \
        decl->getQualifiedNameAsString().find(nsFilter) != std::string::npos) \
      wrapper.addSymbol(decl);                                                \
    return true;                                                              \
  }

  VISIT_DECL(Tag);
  VISIT_DECL(TypedefName);
  VISIT_DECL(Function);
#undef VISIT_DECL
};

class CreateModule : public ASTConsumer {
 private:
  fs::path path;

 public:
  CreateModule(const fs::path &path) : path(fs::canonical(path)) {}

  void HandleTranslationUnit(ASTContext &ctx) override {
    ModuleWrapper wrapper(path);
    FindAllSymbols visitor(wrapper);
    visitor.TraverseDecl(ctx.getTranslationUnitDecl());
    auto modulePath = output.getValue() / path.stem() += ".cppm";
    fs::create_directories(modulePath.parent_path());
    std::ofstream os{modulePath, std::ios::binary};
    os.exceptions(std::ios::failbit);
    os << wrapper.getContent();
  }
};

template <class Consumer>
class SuppressIncludeNotFound : public ASTFrontendAction {
 public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(
      CompilerInstance &, llvm::StringRef path) override {
    return std::make_unique<Consumer>(path.str());
  }
};

int main(int argc, const char **argv) {
  output.setInitialValue(fs::current_path().generic_string());
  auto expectedParser = CommonOptionsParser::create(argc, argv, toolCategory);
  if (!expectedParser) {
    llvm::errs() << expectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &op = expectedParser.get();
  ClangTool tool(op.getCompilations(), op.getSourcePathList());
  return tool.run(
      newFrontendActionFactory<SuppressIncludeNotFound<CreateModule>>().get());
}

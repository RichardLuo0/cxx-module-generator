#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
  fs::path originalFile;

  struct Ns {
    std::unordered_set<std::string> symbols;
    std::unordered_map<std::string, Ns> nss;
  } topLevel;

 public:
  ModuleWrapper(const fs::path &file) : originalFile(file) {}

  void addSymbol(NamedDecl *decl) {
    if (decl == nullptr || !decl->isFirstDecl()) return;

    auto qName = decl->getQualifiedNameAsString();
    std::deque<const std::string_view> qNameSplit;
    for (const auto ns : std::views::split(qName, std::string_view("::"))) {
      std::string_view nsSv(ns.begin(), ns.end());
      if (nsSv != "(anonymous namespace)") qNameSplit.emplace_back(nsSv);
    }
    std::string symbol;
    std::reference_wrapper<Ns> cur = topLevel;
    for (auto &ns : qNameSplit | std::views::take(qNameSplit.size() - 1)) {
      cur = cur.get().nss[std::string(ns)];
      symbol += std::string(ns) + "::";
    }
    symbol += qNameSplit.back();

    if (decl->getLinkageInternal() == Linkage::Internal) {
      llvm::errs() << qName + " has internal linkage. Skipping.\n";
    } else
      cur.get().symbols.emplace("using " + symbol);
  }

 private:
  std::string nsToString(const Ns &ns) const {
    std::string content;
    for (const std::string &symbol : ns.symbols) {
      content += "export " + symbol + ";\n";
    }
    for (const auto &ns : ns.nss) {
      content += "namespace " + ns.first + " {\n";
      content += nsToString(ns.second);
      content += "}\n";
    }
    return content;
  }

 public:
  std::string toString() const {
    std::string content;
    content += "module;\n#include \"" + originalFile.generic_string() + "\"\n";
    content += "export module " +
               (!moduleName.empty() ? moduleName.getValue()
                                    : originalFile.stem().string()) +
               ";\n";
    content += nsToString(topLevel);
    return content;
  }
};

class FindAllSymbols : public RecursiveASTVisitor<FindAllSymbols> {
 private:
  using Base = RecursiveASTVisitor<FindAllSymbols>;

  ModuleWrapper &wrapper;

 public:
  FindAllSymbols(ModuleWrapper &wrapper) : wrapper(wrapper) {}

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
  VISIT_DECL(Var);
  VISIT_DECL(Template);
#undef VISIT_DECL
};

class CreateModule : public ASTConsumer {
 private:
  fs::path modulePath;
  ModuleWrapper wrapper;
  FindAllSymbols visitor;

 public:
  CreateModule(const fs::path &path)
      : modulePath(output.getValue() / path.stem() += ".cppm"),
        wrapper(fs::canonical(path)),
        visitor(wrapper) {}

  ~CreateModule() {
    fs::create_directories(modulePath.parent_path());
    std::ofstream os{modulePath, std::ios::binary};
    os.exceptions(std::ios::failbit);
    os << wrapper.toString();
  }

  void HandleTranslationUnit(ASTContext &ctx) override {
    visitor.TraverseDecl(ctx.getTranslationUnitDecl());
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

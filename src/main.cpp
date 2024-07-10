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
static llvm::cl::opt<bool> ilHeader(
    "internal-linkage-as-header",
    llvm::cl::desc("Generate internal linkage header"),
    llvm::cl::cat(toolCategory));

class ModuleWrapper {
 private:
  const fs::path file;

  struct Ns {
    std::unordered_set<std::string> symbols;
    std::unordered_map<std::string, Ns> nss;

    Ns &operator[](const auto &name) { return nss[std::string(name)]; }

    Ns &getNs(const auto &nss) {
      std::reference_wrapper<Ns> cur = *this;
      for (auto &ns : nss) {
        cur = cur.get()[ns];
      }
      return cur;
    }

    std::string toString(bool isExport = true) const {
      std::string content;
      for (const std::string &symbol : symbols) {
        content += (isExport ? "export " : "") + symbol + ";\n";
      }
      for (const auto &ns : nss) {
        content += "namespace " + ns.first + " {\n";
        content += ns.second.toString(isExport);
        content += "}\n";
      }
      return content;
    }
  } topLevel, ilNs;

 public:
  ModuleWrapper(const fs::path &file) : file(file) {}

  void addSymbol(NamedDecl *decl) {
    if (decl == nullptr || !decl->isFirstDecl()) return;

    auto qName = decl->getQualifiedNameAsString();
    std::deque<const std::string_view> qNameSplit;
    for (const auto ns : std::views::split(qName, std::string_view("::"))) {
      std::string_view nsSv(ns.begin(), ns.end());
      if (nsSv != "(anonymous namespace)") qNameSplit.emplace_back(nsSv);
    }
    const auto qNameNs = qNameSplit | std::views::take(qNameSplit.size() - 1);

    if (decl->getLinkageInternal() == Linkage::Internal) {
      if (ilHeader) {
        auto &ns = ilNs.getNs(qNameNs);
        std::string symbol;
        llvm::raw_string_ostream rso(symbol);
        decl->print(rso);
        ns.symbols.emplace(symbol);
      } else
        llvm::errs() << qName + " has internal linkage. Skipping.\n";
    } else {
      auto &ns = topLevel.getNs(qNameNs);
      std::stringstream symbol;
      std::ranges::copy(qNameNs,
                        std::ostream_iterator<std::string_view>(symbol, "::"));
      symbol << qNameSplit.back();
      ns.symbols.emplace("using " + symbol.str());
    }
  }

  std::string toString(const std::string &name) const {
    std::string content;
    content += "module;\n#include \"" + file.generic_string() + "\"\n";
    content += "export module " + name + ";\n";
    content += topLevel.toString();
    return content;
  }

  std::string ilToString() const {
    std::string content;
    content += ilNs.toString(false);
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
  const fs::path modulePath;
  ModuleWrapper wrapper;
  FindAllSymbols visitor;

 public:
  CreateModule(CompilerInstance &, const fs::path &path)
      : modulePath(output.getValue() / path.stem() += ".cppm"),
        wrapper(fs::canonical(path)),
        visitor(wrapper) {}

  ~CreateModule() {
    fs::create_directories(modulePath.parent_path());

    std::ofstream os{modulePath, std::ios::binary};
    os.exceptions(std::ios::failbit);
    os << wrapper.toString(!moduleName.empty()
                               ? moduleName.getValue()
                               : modulePath.stem().generic_string());
    if (ilHeader) {
      std::ofstream os{fs::path(modulePath).replace_extension("hpp"),
                       std::ios::binary};
      os.exceptions(std::ios::failbit);
      os << wrapper.ilToString();
    }
  }

  void HandleTranslationUnit(ASTContext &ctx) override {
    visitor.TraverseDecl(ctx.getTranslationUnitDecl());
  }
};

template <class Consumer>
class CommonFA : public ASTFrontendAction {
 public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(
      CompilerInstance &ci, llvm::StringRef path) override {
    return std::make_unique<Consumer>(ci, path.str());
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
  return tool.run(newFrontendActionFactory<CommonFA<CreateModule>>().get());
}

//===--- Refactoring.cpp - Framework for clang refactoring tools ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  Implements tools to support refactorings.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Refactoring.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_os_ostream.h"

namespace clang {
namespace tooling {

RefactoringTool::RefactoringTool(
    const CompilationDatabase &Compilations, ArrayRef<std::string> SourcePaths,
    std::shared_ptr<PCHContainerOperations> PCHContainerOps)
    : ClangTool(Compilations, SourcePaths, PCHContainerOps) {}

Replacements &RefactoringTool::getReplacements() { return Replace; }

int RefactoringTool::runAndSave(FrontendActionFactory *ActionFactory) {
  if (int Result = run(ActionFactory)) {
    return Result;
  }

  LangOptions DefaultLangOptions;
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
  TextDiagnosticPrinter DiagnosticPrinter(llvm::errs(), &*DiagOpts);
  DiagnosticsEngine Diagnostics(
      IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()),
      &*DiagOpts, &DiagnosticPrinter, false);
  SourceManager Sources(Diagnostics, getFiles());
  Rewriter Rewrite(Sources, DefaultLangOptions);

  if (!applyAllReplacements(Rewrite)) {
    llvm::errs() << "Skipped some replacements.\n";
  }

  return saveRewrittenFiles(Rewrite);
}

bool RefactoringTool::applyAllReplacements(Rewriter &Rewrite) {
  return tooling::applyAllReplacements(Replace, Rewrite);
}

int RefactoringTool::saveRewrittenFiles(Rewriter &Rewrite) {
  return Rewrite.overwriteChangedFiles() ? 1 : 0;
}

bool formatAndApplyAllReplacements(const Replacements &Replaces,
                                   Rewriter &Rewrite, StringRef Style) {
  SourceManager &SM = Rewrite.getSourceMgr();
  FileManager &Files = SM.getFileManager();

  auto FileToReplaces = groupReplacementsByFile(Replaces);

  bool Result = true;
  for (const auto &FileAndReplaces : FileToReplaces) {
    const std::string &FilePath = FileAndReplaces.first;
    auto &CurReplaces = FileAndReplaces.second;

    const FileEntry *Entry = Files.getFile(FilePath);
    FileID ID = SM.getOrCreateFileID(Entry, SrcMgr::C_User);
    StringRef Code = SM.getBufferData(ID);

    format::FormatStyle CurStyle = format::getStyle(Style, FilePath, "LLVM");
    Replacements NewReplacements =
        format::formatReplacements(Code, CurReplaces, CurStyle);
    Result = applyAllReplacements(NewReplacements, Rewrite) && Result;
  }
  return Result;
}

} // end namespace tooling
} // end namespace clang

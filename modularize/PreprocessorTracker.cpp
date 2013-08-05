//===--- PreprocessorTracker.cpp - Preprocessor tracking -*- C++ -*------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------===//
//
// The Basic Idea
//
// Basically we install a PPCallbacks-derived object to track preprocessor
// activity, namely when a header file is entered/exited, when a macro
// is expanded, when "defined" is used, and when #if, #elif, #ifdef,
// and #ifndef are used.  We save the state of macro and "defined"
// expressions in a map, keyed on a name/file/line/column quadruple.
// The map entries store the different states (values) that a macro expansion,
// "defined" expression, or condition expression has in the course of
// processing for the one location in the one header containing it,
// plus a list of the nested include stacks for the states.  When a macro
// or "defined" expression evaluates to the same value, which is the
// desired case, only one state is stored.  Similarly, for conditional
// directives, we save the condition expression states in a separate map.
//
// This information is collected as modularize compiles all the headers
// given to it to process.  After all the compilations are performed,
// a check is performed for any entries in the maps that contain more
// than one different state, and for these an output message is generated.
//
// For example:
//
//   (...)/SubHeader.h:11:5:
//   #if SYMBOL == 1
//       ^
//   error: Macro instance 'SYMBOL' has different values in this header,
//          depending on how it was included.
//     'SYMBOL' expanded to: '1' with respect to these inclusion paths:
//       (...)/Header1.h
//         (...)/SubHeader.h
//   (...)/SubHeader.h:3:9:
//   #define SYMBOL 1
//             ^
//   Macro defined here.
//     'SYMBOL' expanded to: '2' with respect to these inclusion paths:
//       (...)/Header2.h
//           (...)/SubHeader.h
//   (...)/SubHeader.h:7:9:
//   #define SYMBOL 2
//             ^
//   Macro defined here.
//
// Design and Implementation Details
//
// A PreprocessorTrackerImpl class implements the PreprocessorTracker
// interface. It uses a PreprocessorCallbacks class derived from PPCallbacks
// to track preprocessor activity, namely entering/exiting a header, macro
// expansions, use of "defined" expressions, and #if, #elif, #ifdef, and
// #ifndef conditional directives. PreprocessorTrackerImpl stores a map
// of MacroExpansionTracker objects keyed on a name/file/line/column
// value represented by a light-weight PPItemKey value object. This
// is the key top-level data structure tracking the values of macro
// expansion instances.  Similarly, it stores a map of ConditionalTracker
// objects with the same kind of key, for tracking preprocessor conditional
// directives.
//
// The MacroExpansionTracker object represents one macro reference or use
// of a "defined" expression in a header file. It stores a handle to a
// string representing the unexpanded macro instance, a handle to a string
// representing the unpreprocessed source line containing the unexpanded
// macro instance, and a vector of one or more MacroExpansionInstance
// objects.
//
// The MacroExpansionInstance object represents one or more expansions
// of a macro reference, for the case where the macro expands to the same
// value. MacroExpansionInstance stores a handle to a string representing
// the expanded macro value, a PPItemKey representing the file/line/column
// where the macro was defined, a handle to a string representing the source
// line containing the macro definition, and a vector of InclusionPathHandle
// values that represents the hierarchies of include files for each case 
// where the particular header containing the macro reference was referenced
// or included.

// In the normal case where a macro instance always expands to the same
// value, the MacroExpansionTracker object will only contain one
// MacroExpansionInstance representing all the macro expansion instances.
// If a case was encountered where a macro instance expands to a value
// that is different from that seen before, or the macro was defined in
// a different place, a new MacroExpansionInstance object representing
// that case will be added to the vector in MacroExpansionTracker. If a
// macro instance expands to a value already seen before, the
// InclusionPathHandle representing that case's include file hierarchy
// will be added to the existing MacroExpansionInstance object.

// For checking conditional directives, the ConditionalTracker class
// functions similarly to MacroExpansionTracker, but tracks an #if,
// #elif, #ifdef, or #ifndef directive in a header file.  It stores
// a vector of one or two ConditionalExpansionInstance objects,
// representing the cases where the conditional expression evaluates
// to true or false.  This latter object stores the evaluated value
// of the condition expression (a bool) and a vector of
// InclusionPathHandles.
//
// To reduce the instances of string and object copying, the
// PreprocessorTrackerImpl class uses a StringPool to save all stored
// strings, and defines a StringHandle type to abstract the references
// to the strings.
//
// PreprocessorTrackerImpl also maintains a list representing the unique
// headers, which is just a vector of StringHandle's for the header file
// paths. A HeaderHandle abstracts a reference to a header, and is simply
// the index of the stored header file path.
//
// A HeaderInclusionPath class abstracts a unique hierarchy of header file
// inclusions. It simply stores a vector of HeaderHandles ordered from the
// top-most header (the one from the header list passed to modularize) down
// to the header containing the macro reference. PreprocessorTrackerImpl
// stores a vector of these objects. An InclusionPathHandle typedef
// abstracts a reference to one of the HeaderInclusionPath objects, and is
// simply the index of the stored HeaderInclusionPath object. The
// MacroExpansionInstance object stores a vector of these handles so that
// the reporting function can display the include hierarchies for the macro
// expansion instances represented by that object, to help the user
// understand how the header was included. (A future enhancement might
// be to associate a line number for the #include directives, but I
// think not doing so is good enough for the present.)
//
// A key reason for using these opaque handles was to try to keep all the
// internal objects light-weight value objects, in order to reduce string
// and object copying overhead, and to abstract this implementation detail.
//
// The key data structures are built up while modularize runs the headers
// through the compilation. A PreprocessorTracker instance is created and
// passed down to the AST action and consumer objects in modularize. For
// each new compilation instance, the consumer calls the
// PreprocessorTracker's handleNewPreprocessorEntry function, which sets
// up a PreprocessorCallbacks object for the preprocessor. At the end of
// the compilation instance, the PreprocessorTracker's
// handleNewPreprocessorExit function handles cleaning up with respect
// to the preprocessing instance.
//
// The PreprocessorCallbacks object uses an overidden FileChanged callback
// to determine when a header is entered and exited (including exiting the
// header during #include directives). It calls PreprocessorTracker's
// handleHeaderEntry and handleHeaderExit functions upon entering and
// exiting a header. These functions manage a stack of header handles
// representing by a vector, pushing and popping header handles as headers
// are entered and exited. When a HeaderInclusionPath object is created,
// it simply copies this stack.
//
// The PreprocessorCallbacks object uses an overridden MacroExpands callback
// to track when a macro expansion is performed. It calls a couple of helper
// functions to get the unexpanded and expanded macro values as strings, but
// then calls PreprocessorTrackerImpl's addMacroExpansionInstance function to
// do the rest of the work. The getMacroExpandedString function uses the
// preprocessor's getSpelling to convert tokens to strings using the
// information passed to the MacroExpands callback, and simply concatenates
// them. It makes recursive calls to itself to handle nested macro
// definitions, and also handles function-style macros.
//
// PreprocessorTrackerImpl's addMacroExpansionInstance function looks for
// an existing MacroExpansionTracker entry in its map of MacroExampleTracker
// objects. If none exists, it adds one with one MacroExpansionInstance and
// returns. If a MacroExpansionTracker object already exists, it looks for
// an existing MacroExpansionInstance object stored in the
// MacroExpansionTracker object, one that matches the macro expanded value
// and the macro definition location. If a matching MacroExpansionInstance
// object is found, it just adds the current HeaderInclusionPath object to
// it. If not found, it creates and stores a new MacroExpantionInstance
// object. The addMacroExpansionInstance function calls a couple of helper
// functions to get the pre-formatted location and source line strings for
// the macro reference and the macro definition stored as string handles.
// These helper functions use the current source manager from the
// preprocessor. This is done in advance at this point in time because the
// source manager doesn't exist at the time of the reporting.
//
// For conditional check, the PreprocessorCallbacks class overrides the
// PPCallbacks handlers for #if, #elif, #ifdef, and #ifndef.  These handlers
// call the addConditionalExpansionInstance method of
// PreprocessorTrackerImpl.  The process is similar to that of macros, but
// with some different data and error messages.  A lookup is performed for
// the conditional, and if a ConditionalTracker object doesn't yet exist for
// the conditional, a new one is added, including adding a
// ConditionalExpansionInstance object to it to represent the condition
// expression state.  If a ConditionalTracker for the conditional does
// exist, a lookup is made for a ConditionalExpansionInstance object
// matching the condition expression state.  If one exists, a
// HeaderInclusionPath is added to it.  Otherwise a new
// ConditionalExpansionInstance  entry is made.  If a ConditionalTracker
// has two ConditionalExpansionInstance objects, it means there was a
// conflict, meaning the conditional expression evaluated differently in
// one or more cases.
// 
// After modularize has performed all the compilations, it enters a phase
// of error reporting. This new feature adds to this reporting phase calls
// to the PreprocessorTracker's reportInconsistentMacros and
// reportInconsistentConditionals functions. These functions walk the maps
// of MacroExpansionTracker's and ConditionalTracker's respectively. If
// any of these objects have more than one MacroExpansionInstance or
// ConditionalExpansionInstance objects, it formats and outputs an error
// message like the example shown previously, using the stored data.
//
// A potential issue is that there is some overlap between the #if/#elif
// conditional and macro reporting.  I could disable the #if and #elif,
// leaving just the #ifdef and #ifndef, since these don't overlap.  Or,
// to make clearer the separate reporting phases, I could add an output
// message marking the phases.
//
// Future Directions
//
// We probably should add options to disable any of the checks, in case
// there is some problem with them, or the messages get too verbose.
//
// With the map of all the macro and conditional expansion instances,
// it might be possible to add to the existing modularize error messages
// (the second part referring to definitions being different), attempting
// to tie them to the last macro conflict encountered with respect to the
// order of the code encountered.
//
//===--------------------------------------------------------------------===//

#include "clang/Lex/LexDiagnostic.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/StringPool.h"
#include "PreprocessorTracker.h"

namespace Modularize {

// Forwards.
class PreprocessorTrackerImpl;

// Some handle types
typedef llvm::PooledStringPtr StringHandle;

typedef int HeaderHandle;
const HeaderHandle HeaderHandleInvalid = -1;

typedef int InclusionPathHandle;
const InclusionPathHandle InclusionPathHandleInvalid = -1;

// Some utility functions.

// Get a "file:line:column" source location string.
static std::string getSourceLocationString(clang::Preprocessor &PP,
                                           clang::SourceLocation Loc) {
  if (Loc.isInvalid())
    return std::string("(none)");
  else
    return Loc.printToString(PP.getSourceManager());
}

// Get just the file name from a source location.
static std::string getSourceLocationFile(clang::Preprocessor &PP,
                                         clang::SourceLocation Loc) {
  std::string Source(getSourceLocationString(PP, Loc));
  size_t Offset = Source.find(':', 2);
  if (Offset == std::string::npos)
    return Source;
  return Source.substr(0, Offset);
}

// Get just the line and column from a source location.
static void getSourceLocationLineAndColumn(clang::Preprocessor &PP,
                                           clang::SourceLocation Loc, int &Line,
                                           int &Column) {
  clang::PresumedLoc PLoc = PP.getSourceManager().getPresumedLoc(Loc);
  if (PLoc.isInvalid()) {
    Line = 0;
    Column = 0;
    return;
  }
  Line = PLoc.getLine();
  Column = PLoc.getColumn();
}

// Retrieve source snippet from file image.
std::string getSourceString(clang::Preprocessor &PP, clang::SourceRange Range) {
  clang::SourceLocation BeginLoc = Range.getBegin();
  clang::SourceLocation EndLoc = Range.getEnd();
  const char *BeginPtr = PP.getSourceManager().getCharacterData(BeginLoc);
  const char *EndPtr = PP.getSourceManager().getCharacterData(EndLoc);
  size_t Length = EndPtr - BeginPtr;
  return llvm::StringRef(BeginPtr, Length).trim().str();
}

// Retrieve source line from file image.
std::string getSourceLine(clang::Preprocessor &PP, clang::SourceLocation Loc) {
  const llvm::MemoryBuffer *MemBuffer =
      PP.getSourceManager().getBuffer(PP.getSourceManager().getFileID(Loc));
  const char *Buffer = MemBuffer->getBufferStart();
  const char *BufferEnd = MemBuffer->getBufferEnd();
  const char *BeginPtr = PP.getSourceManager().getCharacterData(Loc);
  const char *EndPtr = BeginPtr;
  while (BeginPtr > Buffer) {
    if (*BeginPtr == '\n') {
      BeginPtr++;
      break;
    }
    BeginPtr--;
  }
  while (EndPtr < BufferEnd) {
    if (*EndPtr == '\n') {
      break;
    }
    EndPtr++;
  }
  size_t Length = EndPtr - BeginPtr;
  return llvm::StringRef(BeginPtr, Length).str();
}

// Get the string for the Unexpanded macro instance.
// The soureRange is expected to end at the last token
// for the macro instance, which in the case of a function-style
// macro will be a ')', but for an object-style macro, it
// will be the macro name itself.
std::string getMacroUnexpandedString(clang::SourceRange Range,
                                     clang::Preprocessor &PP,
                                     llvm::StringRef MacroName,
                                     const clang::MacroInfo *MI) {
  clang::SourceLocation BeginLoc(Range.getBegin());
  const char *BeginPtr = PP.getSourceManager().getCharacterData(BeginLoc);
  size_t Length;
  std::string Unexpanded;
  if (MI->isFunctionLike()) {
    clang::SourceLocation EndLoc(Range.getEnd());
    const char *EndPtr = PP.getSourceManager().getCharacterData(EndLoc) + 1;
    Length = (EndPtr - BeginPtr) + 1; // +1 is ')' width.
  } else
    Length = MacroName.size();
  return llvm::StringRef(BeginPtr, Length).trim().str();
}

// Get the expansion for a macro instance, given the information
// provided by PPCallbacks.
std::string getMacroExpandedString(clang::Preprocessor &PP,
                                   llvm::StringRef MacroName,
                                   const clang::MacroInfo *MI,
                                   const clang::MacroArgs *Args) {
  std::string Expanded;
  // Walk over the macro Tokens.
  typedef clang::MacroInfo::tokens_iterator Iter;
  for (Iter I = MI->tokens_begin(), E = MI->tokens_end(); I != E; ++I) {
    clang::IdentifierInfo *II = I->getIdentifierInfo();
    int ArgNo = (II && Args ? MI->getArgumentNum(II) : -1);
    if (ArgNo == -1) {
      // This isn't an argument, just add it.
      if (II == NULL)
        Expanded += PP.getSpelling((*I)); // Not an identifier.
      else {
        // Token is for an identifier.
        std::string Name = II->getName().str();
        // Check for nexted macro references.
        clang::MacroInfo *MacroInfo = PP.getMacroInfo(II);
        if (MacroInfo != NULL)
          Expanded += getMacroExpandedString(PP, Name, MacroInfo, NULL);
        else
          Expanded += Name;
      }
      continue;
    }
    // We get here if it's a function-style macro with arguments.
    const clang::Token *ResultArgToks;
    const clang::Token *ArgTok = Args->getUnexpArgument(ArgNo);
    if (Args->ArgNeedsPreexpansion(ArgTok, PP))
      ResultArgToks = &(const_cast<clang::MacroArgs *>(Args))
          ->getPreExpArgument(ArgNo, MI, PP)[0];
    else
      ResultArgToks = ArgTok; // Use non-preexpanded Tokens.
    // If the arg token didn't expand into anything, ignore it.
    if (ResultArgToks->is(clang::tok::eof))
      continue;
    unsigned NumToks = clang::MacroArgs::getArgLength(ResultArgToks);
    // Append the resulting argument expansions.
    for (unsigned ArgumentIndex = 0; ArgumentIndex < NumToks; ++ArgumentIndex) {
      const clang::Token &AT = ResultArgToks[ArgumentIndex];
      clang::IdentifierInfo *II = AT.getIdentifierInfo();
      if (II == NULL)
        Expanded += PP.getSpelling(AT); // Not an identifier.
      else {
        // It's an identifier.  Check for further expansion.
        std::string Name = II->getName().str();
        clang::MacroInfo *MacroInfo = PP.getMacroInfo(II);
        if (MacroInfo != NULL)
          Expanded += getMacroExpandedString(PP, Name, MacroInfo, NULL);
        else
          Expanded += Name;
      }
    }
  }
  return Expanded;
}

// Get the string representing a vector of Tokens.
std::string
getTokensSpellingString(clang::Preprocessor &PP,
                        llvm::SmallVectorImpl<clang::Token> &Tokens) {
  std::string Expanded;
  // Walk over the macro Tokens.
  typedef llvm::SmallVectorImpl<clang::Token>::iterator Iter;
  for (Iter I = Tokens.begin(), E = Tokens.end(); I != E; ++I)
    Expanded += PP.getSpelling(*I); // Not an identifier.
  return llvm::StringRef(Expanded).trim().str();
}

// Get the expansion for a macro instance, given the information
// provided by PPCallbacks.
std::string getExpandedString(clang::Preprocessor &PP,
                              llvm::StringRef MacroName,
                              const clang::MacroInfo *MI,
                              const clang::MacroArgs *Args) {
  std::string Expanded;
  // Walk over the macro Tokens.
  typedef clang::MacroInfo::tokens_iterator Iter;
  for (Iter I = MI->tokens_begin(), E = MI->tokens_end(); I != E; ++I) {
    clang::IdentifierInfo *II = I->getIdentifierInfo();
    int ArgNo = (II && Args ? MI->getArgumentNum(II) : -1);
    if (ArgNo == -1) {
      // This isn't an argument, just add it.
      if (II == NULL)
        Expanded += PP.getSpelling((*I)); // Not an identifier.
      else {
        // Token is for an identifier.
        std::string Name = II->getName().str();
        // Check for nexted macro references.
        clang::MacroInfo *MacroInfo = PP.getMacroInfo(II);
        if (MacroInfo != NULL)
          Expanded += getMacroExpandedString(PP, Name, MacroInfo, NULL);
        else
          Expanded += Name;
      }
      continue;
    }
    // We get here if it's a function-style macro with arguments.
    const clang::Token *ResultArgToks;
    const clang::Token *ArgTok = Args->getUnexpArgument(ArgNo);
    if (Args->ArgNeedsPreexpansion(ArgTok, PP))
      ResultArgToks = &(const_cast<clang::MacroArgs *>(Args))
          ->getPreExpArgument(ArgNo, MI, PP)[0];
    else
      ResultArgToks = ArgTok; // Use non-preexpanded Tokens.
    // If the arg token didn't expand into anything, ignore it.
    if (ResultArgToks->is(clang::tok::eof))
      continue;
    unsigned NumToks = clang::MacroArgs::getArgLength(ResultArgToks);
    // Append the resulting argument expansions.
    for (unsigned ArgumentIndex = 0; ArgumentIndex < NumToks; ++ArgumentIndex) {
      const clang::Token &AT = ResultArgToks[ArgumentIndex];
      clang::IdentifierInfo *II = AT.getIdentifierInfo();
      if (II == NULL)
        Expanded += PP.getSpelling(AT); // Not an identifier.
      else {
        // It's an identifier.  Check for further expansion.
        std::string Name = II->getName().str();
        clang::MacroInfo *MacroInfo = PP.getMacroInfo(II);
        if (MacroInfo != NULL)
          Expanded += getMacroExpandedString(PP, Name, MacroInfo, NULL);
        else
          Expanded += Name;
      }
    }
  }
  return Expanded;
}

// We need some operator overloads for string handles.
bool operator==(const StringHandle &H1, const StringHandle &H2) {
  const char *S1 = (H1 ? *H1 : "");
  const char *S2 = (H2 ? *H2 : "");
  int Diff = strcmp(S1, S2);
  return Diff == 0;
}
bool operator!=(const StringHandle &H1, const StringHandle &H2) {
  const char *S1 = (H1 ? *H1 : "");
  const char *S2 = (H2 ? *H2 : "");
  int Diff = strcmp(S1, S2);
  return Diff != 0;
}
bool operator<(const StringHandle &H1, const StringHandle &H2) {
  const char *S1 = (H1 ? *H1 : "");
  const char *S2 = (H2 ? *H2 : "");
  int Diff = strcmp(S1, S2);
  return Diff < 0;
}
bool operator>(const StringHandle &H1, const StringHandle &H2) {
  const char *S1 = (H1 ? *H1 : "");
  const char *S2 = (H2 ? *H2 : "");
  int Diff = strcmp(S1, S2);
  return Diff > 0;
}

// Preprocessor item key.
//
// This class represents a location in a source file, for use
// as a key representing a unique name/file/line/column quadruplet,
// which in this case is used to identify a macro expansion instance,
// but could be used for other things as well.
// The file is a header file handle, the line is a line number,
// and the column is a column number.
class PPItemKey {
public:
  PPItemKey(clang::Preprocessor &PP, StringHandle Name, HeaderHandle File,
            clang::SourceLocation Loc)
      : Name(Name), File(File) {
    getSourceLocationLineAndColumn(PP, Loc, Line, Column);
  }
  PPItemKey(StringHandle Name, HeaderHandle File, int Line, int Column)
      : Name(Name), File(File), Line(Line), Column(Column) {}
  PPItemKey(const PPItemKey &Other)
      : Name(Other.Name), File(Other.File), Line(Other.Line),
        Column(Other.Column) {}
  PPItemKey() : File(HeaderHandleInvalid), Line(0), Column(0) {}
  bool operator==(const PPItemKey &Other) const {
    if (Name != Other.Name)
      return false;
    if (File != Other.File)
      return false;
    if (Line != Other.Line)
      return false;
    return Column == Other.Column;
  }
  bool operator<(const PPItemKey &Other) const {
    if (Name < Other.Name)
      return true;
    else if (Name > Other.Name)
      return false;
    if (File < Other.File)
      return true;
    else if (File > Other.File)
      return false;
    if (Line < Other.Line)
      return true;
    else if (Line > Other.Line)
      return false;
    return Column < Other.Column;
  }
  StringHandle Name;
  HeaderHandle File;
  int Line;
  int Column;
};

// Header inclusion path.
class HeaderInclusionPath {
public:
  HeaderInclusionPath(std::vector<HeaderHandle> HeaderInclusionPath)
      : Path(HeaderInclusionPath) {}
  HeaderInclusionPath(const HeaderInclusionPath &Other) : Path(Other.Path) {}
  HeaderInclusionPath() {}
  std::vector<HeaderHandle> Path;
};

// Macro expansion instance.
//
// This class represents an instance of a macro expansion with a
// unique value.  It also stores the unique header inclusion paths
// for use in telling the user the nested include path to the header.
class MacroExpansionInstance {
public:
  MacroExpansionInstance(StringHandle MacroExpanded,
                         PPItemKey &DefinitionLocation,
                         StringHandle DefinitionSourceLine,
                         InclusionPathHandle H)
      : MacroExpanded(MacroExpanded), DefinitionLocation(DefinitionLocation),
        DefinitionSourceLine(DefinitionSourceLine) {
    InclusionPathHandles.push_back(H);
  }
  MacroExpansionInstance() {}

  // Check for the presence of a header inclusion path handle entry.
  // Return false if not found.
  bool haveInclusionPathHandle(InclusionPathHandle H) {
    for (std::vector<InclusionPathHandle>::iterator
             I = InclusionPathHandles.begin(),
             E = InclusionPathHandles.end();
         I != E; ++I) {
      if (*I == H)
        return true;
    }
    return InclusionPathHandleInvalid;
  }
  // Add a new header inclusion path entry, if not already present.
  void addInclusionPathHandle(InclusionPathHandle H) {
    if (!haveInclusionPathHandle(H))
      InclusionPathHandles.push_back(H);
  }

  // A string representing the macro instance after preprocessing.
  StringHandle MacroExpanded;
  // A file/line/column triplet representing the macro definition location.
  PPItemKey DefinitionLocation;
  // A place to save the macro definition line string.
  StringHandle DefinitionSourceLine;
  // The header inclusion path handles for all the instances.
  std::vector<InclusionPathHandle> InclusionPathHandles;
};

// Macro expansion instance tracker.
//
// This class represents one macro expansion, keyed by a PPItemKey.
// It stores a string representing the macro reference in the source,
// and a list of ConditionalExpansionInstances objects representing
// the unique values the condition expands to in instances of the header.
class MacroExpansionTracker {
public:
  MacroExpansionTracker(StringHandle MacroUnexpanded,
                        StringHandle MacroExpanded,
                        StringHandle InstanceSourceLine,
                        PPItemKey &DefinitionLocation,
                        StringHandle DefinitionSourceLine,
                        InclusionPathHandle InclusionPathHandle)
      : MacroUnexpanded(MacroUnexpanded),
        InstanceSourceLine(InstanceSourceLine) {
    addMacroExpansionInstance(MacroExpanded, DefinitionLocation,
                              DefinitionSourceLine, InclusionPathHandle);
  }
  MacroExpansionTracker() {}

  // Find a matching macro expansion instance.
  MacroExpansionInstance *
  findMacroExpansionInstance(StringHandle MacroExpanded,
                             PPItemKey &DefinitionLocation) {
    for (std::vector<MacroExpansionInstance>::iterator
             I = MacroExpansionInstances.begin(),
             E = MacroExpansionInstances.end();
         I != E; ++I) {
      if ((I->MacroExpanded == MacroExpanded) &&
          (I->DefinitionLocation == DefinitionLocation)) {
        return &*I; // Found.
      }
    }
    return NULL; // Not found.
  }

  // Add a macro expansion instance.
  void addMacroExpansionInstance(StringHandle MacroExpanded,
                                 PPItemKey &DefinitionLocation,
                                 StringHandle DefinitionSourceLine,
                                 InclusionPathHandle InclusionPathHandle) {
    MacroExpansionInstances.push_back(
        MacroExpansionInstance(MacroExpanded, DefinitionLocation,
                               DefinitionSourceLine, InclusionPathHandle));
  }

  // Return true if there is a mismatch.
  bool hasMismatch() { return MacroExpansionInstances.size() > 1; }

  // A string representing the macro instance without expansion.
  StringHandle MacroUnexpanded;
  // A place to save the macro instance source line string.
  StringHandle InstanceSourceLine;
  // The macro expansion instances.
  // If all instances of the macro expansion expand to the same value,
  // This vector will only have one instance.
  std::vector<MacroExpansionInstance> MacroExpansionInstances;
};

// Conditional expansion instance.
//
// This class represents an instance of a condition exoression result
// with a unique value.  It also stores the unique header inclusion paths
// for use in telling the user the nested include path to the header.
class ConditionalExpansionInstance {
public:
  ConditionalExpansionInstance(bool ConditionValue, InclusionPathHandle H)
      : ConditionValue(ConditionValue) {
    InclusionPathHandles.push_back(H);
  }
  ConditionalExpansionInstance() {}

  // Check for the presence of a header inclusion path handle entry.
  // Return false if not found.
  bool haveInclusionPathHandle(InclusionPathHandle H) {
    for (std::vector<InclusionPathHandle>::iterator
             I = InclusionPathHandles.begin(),
             E = InclusionPathHandles.end();
         I != E; ++I) {
      if (*I == H)
        return true;
    }
    return InclusionPathHandleInvalid;
  }
  // Add a new header inclusion path entry, if not already present.
  void addInclusionPathHandle(InclusionPathHandle H) {
    if (!haveInclusionPathHandle(H))
      InclusionPathHandles.push_back(H);
  }

  // A flag representing the evaluated condition value.
  bool ConditionValue;
  // The header inclusion path handles for all the instances.
  std::vector<InclusionPathHandle> InclusionPathHandles;
};

// Conditional directive instance tracker.
//
// This class represents one conditional directive, keyed by a PPItemKey.
// It stores a string representing the macro reference in the source,
// and a list of ConditionExpansionInstance objects representing
// the unique value the condition expression expands to in instances of
// the header.
class ConditionalTracker {
public:
  ConditionalTracker(clang::tok::PPKeywordKind DirectiveKind,
                     bool ConditionValue, StringHandle ConditionUnexpanded,
                     InclusionPathHandle InclusionPathHandle)
      : DirectiveKind(DirectiveKind), ConditionUnexpanded(ConditionUnexpanded) {
    addConditionalExpansionInstance(ConditionValue, InclusionPathHandle);
  }
  ConditionalTracker() {}

  // Find a matching condition expansion instance.
  ConditionalExpansionInstance *
  findConditionalExpansionInstance(bool ConditionValue) {
    for (std::vector<ConditionalExpansionInstance>::iterator
             I = ConditionalExpansionInstances.begin(),
             E = ConditionalExpansionInstances.end();
         I != E; ++I) {
      if (I->ConditionValue == ConditionValue) {
        return &*I; // Found.
      }
    }
    return NULL; // Not found.
  }

  // Add a conditional expansion instance.
  void
  addConditionalExpansionInstance(bool ConditionValue,
                                  InclusionPathHandle InclusionPathHandle) {
    ConditionalExpansionInstances.push_back(
        ConditionalExpansionInstance(ConditionValue, InclusionPathHandle));
  }

  // Return true if there is a mismatch.
  bool hasMismatch() { return ConditionalExpansionInstances.size() > 1; }

  // The kind of directive.
  clang::tok::PPKeywordKind DirectiveKind;
  // A string representing the macro instance without expansion.
  StringHandle ConditionUnexpanded;
  // The condition expansion instances.
  // If all instances of the conditional expression expand to the same value,
  // This vector will only have one instance.
  std::vector<ConditionalExpansionInstance> ConditionalExpansionInstances;
};

// Preprocessor callbacks for modularize.
//
// This class derives from the Clang PPCallbacks class to track preprocessor
// actions, such as changing files and handling preprocessor directives and
// macro expansions.  It has to figure out when a new header file is entered
// and left, as the provided handler is not particularly clear about it.
class PreprocessorCallbacks : public clang::PPCallbacks {
public:
  PreprocessorCallbacks(PreprocessorTrackerImpl &ppTracker,
                        clang::Preprocessor &PP, llvm::StringRef rootHeaderFile)
      : PPTracker(ppTracker), PP(PP), RootHeaderFile(rootHeaderFile) {}
  ~PreprocessorCallbacks() {}

  // Overridden handlers.
  void FileChanged(clang::SourceLocation Loc,
                   clang::PPCallbacks::FileChangeReason Reason,
                   clang::SrcMgr::CharacteristicKind FileType,
                   clang::FileID PrevFID = clang::FileID());
  void MacroExpands(const clang::Token &MacroNameTok,
                    const clang::MacroDirective *MD, clang::SourceRange Range,
                    const clang::MacroArgs *Args);
  void Defined(const clang::Token &MacroNameTok,
               const clang::MacroDirective *MD, clang::SourceRange Range);
  void If(clang::SourceLocation Loc, clang::SourceRange ConditionRange,
          bool ConditionResult);
  void Elif(clang::SourceLocation Loc, clang::SourceRange ConditionRange,
            bool ConditionResult, clang::SourceLocation IfLoc);
  void Ifdef(clang::SourceLocation Loc, const clang::Token &MacroNameTok,
             const clang::MacroDirective *MD);
  void Ifndef(clang::SourceLocation Loc, const clang::Token &MacroNameTok,
              const clang::MacroDirective *MD);

private:
  PreprocessorTrackerImpl &PPTracker;
  clang::Preprocessor &PP;
  std::string RootHeaderFile;
};

// Preprocessor macro expansion item map types.
typedef std::map<PPItemKey, MacroExpansionTracker> MacroExpansionMap;
typedef std::map<PPItemKey, MacroExpansionTracker>::iterator
MacroExpansionMapIter;

// Preprocessor conditional expansion item map types.
typedef std::map<PPItemKey, ConditionalTracker> ConditionalExpansionMap;
typedef std::map<PPItemKey, ConditionalTracker>::iterator
ConditionalExpansionMapIter;

// Preprocessor tracker for modularize.
//
// This class stores information about all the headers processed in the
// course of running modularize.
class PreprocessorTrackerImpl : public PreprocessorTracker {
public:
  PreprocessorTrackerImpl()
      : CurrentInclusionPathHandle(InclusionPathHandleInvalid) {}
  ~PreprocessorTrackerImpl() {}

  // Handle entering a preprocessing session.
  void handlePreprocessorEntry(clang::Preprocessor &PP,
                               llvm::StringRef rootHeaderFile) {
    assert((HeaderStack.size() == 0) && "Header stack should be empty.");
    pushHeaderHandle(addHeader(rootHeaderFile));
    PP.addPPCallbacks(new PreprocessorCallbacks(*this, PP, rootHeaderFile));
  }
  // Handle exiting a preprocessing session.
  void handlePreprocessorExit() { HeaderStack.clear(); }

  // Handle entering a header source file.
  void handleHeaderEntry(clang::Preprocessor &PP, llvm::StringRef HeaderPath) {
    // Ignore <built-in> and <command-line> to reduce message clutter.
    if (HeaderPath.startswith("<"))
      return;
    HeaderHandle H = addHeader(HeaderPath);
    if (H != getCurrentHeaderHandle())
      pushHeaderHandle(H);
  }
  // Handle exiting a header source file.
  void handleHeaderExit(llvm::StringRef HeaderPath) {
    // Ignore <built-in> and <command-line> to reduce message clutter.
    if (HeaderPath.startswith("<"))
      return;
    HeaderHandle H = findHeaderHandle(HeaderPath);
    if (isHeaderHandleInStack(H)) {
      while ((H != getCurrentHeaderHandle()) && (HeaderStack.size() != 0))
        popHeaderHandle();
    }
  }

  // Lookup/add string.
  StringHandle addString(llvm::StringRef Str) { return Strings.intern(Str); }

  // Get the handle of a header file entry.
  // Return HeaderHandleInvalid if not found.
  HeaderHandle findHeaderHandle(llvm::StringRef HeaderPath) const {
    HeaderHandle H = 0;
    for (std::vector<StringHandle>::const_iterator I = HeaderPaths.begin(),
                                                   E = HeaderPaths.end();
         I != E; ++I, ++H) {
      if (**I == HeaderPath)
        return H;
    }
    return HeaderHandleInvalid;
  }

  // Add a new header file entry, or return existing handle.
  // Return the header handle.
  HeaderHandle addHeader(llvm::StringRef HeaderPath) {
    std::string canonicalPath(HeaderPath);
    std::replace(canonicalPath.begin(), canonicalPath.end(), '\\', '/');
    HeaderHandle H = findHeaderHandle(canonicalPath);
    if (H == HeaderHandleInvalid) {
      H = HeaderPaths.size();
      HeaderPaths.push_back(addString(canonicalPath));
    }
    return H;
  }

  // Return a header file path string given its handle.
  StringHandle getHeaderFilePath(HeaderHandle H) const {
    if ((H >= 0) && (H < (HeaderHandle)HeaderPaths.size()))
      return HeaderPaths[H];
    return StringHandle();
  }

  // Returns a handle to the inclusion path.
  InclusionPathHandle pushHeaderHandle(HeaderHandle H) {
    HeaderStack.push_back(H);
    return CurrentInclusionPathHandle = addInclusionPathHandle(HeaderStack);
  }
  // Pops the last header handle from the stack;
  void popHeaderHandle() {
    // assert((HeaderStack.size() != 0) && "Header stack already empty.");
    if (HeaderStack.size() != 0) {
      HeaderStack.pop_back();
      CurrentInclusionPathHandle = addInclusionPathHandle(HeaderStack);
    }
  }
  // Get the top handle on the header stack.
  HeaderHandle getCurrentHeaderHandle() const {
    if (HeaderStack.size() != 0)
      return HeaderStack.back();
    return HeaderHandleInvalid;
  }

  // Check for presence of header handle in the header stack.
  bool isHeaderHandleInStack(HeaderHandle H) const {
    for (std::vector<HeaderHandle>::const_iterator I = HeaderStack.begin(),
                                                   E = HeaderStack.end();
         I != E; ++I) {
      if (*I == H)
        return true;
    }
    return false;
  }

  // Get the handle of a header inclusion path entry.
  // Return InclusionPathHandleInvalid if not found.
  InclusionPathHandle
  findInclusionPathHandle(const std::vector<HeaderHandle> &Path) const {
    InclusionPathHandle H = 0;
    for (std::vector<HeaderInclusionPath>::const_iterator
             I = InclusionPaths.begin(),
             E = InclusionPaths.end();
         I != E; ++I, ++H) {
      if (I->Path == Path)
        return H;
    }
    return HeaderHandleInvalid;
  }
  // Add a new header inclusion path entry, or return existing handle.
  // Return the header inclusion path entry handle.
  InclusionPathHandle
  addInclusionPathHandle(const std::vector<HeaderHandle> &Path) {
    InclusionPathHandle H = findInclusionPathHandle(Path);
    if (H == HeaderHandleInvalid) {
      H = InclusionPaths.size();
      InclusionPaths.push_back(HeaderInclusionPath(Path));
    }
    return H;
  }
  // Return the current inclusion path handle.
  InclusionPathHandle getCurrentInclusionPathHandle() const {
    return CurrentInclusionPathHandle;
  }

  // Return an inclusion path given its handle.
  const std::vector<HeaderHandle> &
  getInclusionPath(InclusionPathHandle H) const {
    if ((H >= 0) && (H <= (InclusionPathHandle)InclusionPaths.size()))
      return InclusionPaths[H].Path;
    static std::vector<HeaderHandle> Empty;
    return Empty;
  }

  // Add a macro expansion instance.
  void addMacroExpansionInstance(clang::Preprocessor &PP, HeaderHandle H,
                                 clang::SourceLocation InstanceLoc,
                                 clang::SourceLocation DefinitionLoc,
                                 clang::IdentifierInfo *II,
                                 llvm::StringRef MacroUnexpanded,
                                 llvm::StringRef MacroExpanded,
                                 InclusionPathHandle InclusionPathHandle) {
    StringHandle MacroName = addString(II->getName());
    PPItemKey InstanceKey(PP, MacroName, H, InstanceLoc);
    PPItemKey DefinitionKey(PP, MacroName, H, DefinitionLoc);
    MacroExpansionMapIter I = MacroExpansions.find(InstanceKey);
    // If existing instance of expansion not found, add one.
    if (I == MacroExpansions.end()) {
      std::string InstanceSourceLine =
          getSourceLocationString(PP, InstanceLoc) + ":\n" +
          getSourceLine(PP, InstanceLoc) + "\n";
      std::string DefinitionSourceLine =
          getSourceLocationString(PP, DefinitionLoc) + ":\n" +
          getSourceLine(PP, DefinitionLoc) + "\n";
      MacroExpansions[InstanceKey] = MacroExpansionTracker(
          addString(MacroUnexpanded), addString(MacroExpanded),
          addString(InstanceSourceLine), DefinitionKey,
          addString(DefinitionSourceLine), InclusionPathHandle);
    } else {
      // We've seen the macro before.  Get its tracker.
      MacroExpansionTracker &CondTracker = I->second;
      // Look up an existing instance value for the macro.
      MacroExpansionInstance *MacroInfo =
          CondTracker.findMacroExpansionInstance(addString(MacroExpanded),
                                                 DefinitionKey);
      // If found, just add the inclusion path to the instance.
      if (MacroInfo != NULL)
        MacroInfo->addInclusionPathHandle(InclusionPathHandle);
      else {
        // Otherwise add a new instance with the unique value.
        std::string DefinitionSourceLine =
            getSourceLocationString(PP, DefinitionLoc) + ":\n" +
            getSourceLine(PP, DefinitionLoc) + "\n";
        CondTracker.addMacroExpansionInstance(
            addString(MacroExpanded), DefinitionKey,
            addString(DefinitionSourceLine), InclusionPathHandle);
      }
    }
  }

  // Add a conditional expansion instance.
  void
  addConditionalExpansionInstance(clang::Preprocessor &PP, HeaderHandle H,
                                  clang::SourceLocation InstanceLoc,
                                  clang::tok::PPKeywordKind DirectiveKind,
                                  bool ConditionValue,
                                  llvm::StringRef ConditionUnexpanded,
                                  InclusionPathHandle InclusionPathHandle) {
    StringHandle ConditionUnexpandedHandle(addString(ConditionUnexpanded));
    PPItemKey InstanceKey(PP, ConditionUnexpandedHandle, H, InstanceLoc);
    ConditionalExpansionMapIter I = ConditionalExpansions.find(InstanceKey);
    // If existing instance of condition not found, add one.
    if (I == ConditionalExpansions.end()) {
      std::string InstanceSourceLine =
          getSourceLocationString(PP, InstanceLoc) + ":\n" +
          getSourceLine(PP, InstanceLoc) + "\n";
      ConditionalExpansions[InstanceKey] =
          ConditionalTracker(DirectiveKind, ConditionValue, ConditionUnexpandedHandle,
                             InclusionPathHandle);
    } else {
      // We've seen the conditional before.  Get its tracker.
      ConditionalTracker &CondTracker = I->second;
      // Look up an existing instance value for the condition.
      ConditionalExpansionInstance *MacroInfo =
          CondTracker.findConditionalExpansionInstance(ConditionValue);
      // If found, just add the inclusion path to the instance.
      if (MacroInfo != NULL)
        MacroInfo->addInclusionPathHandle(InclusionPathHandle);
      else {
        // Otherwise add a new instance with the unique value.
        CondTracker.addConditionalExpansionInstance(ConditionValue,
                                                    InclusionPathHandle);
      }
    }
  }

  // Report on inconsistent macro instances.
  // Returns true if any mismatches.
  bool reportInconsistentMacros(llvm::raw_ostream &OS) {
    bool ReturnValue = false;
    // Walk all the macro expansion trackers in the map.
    for (MacroExpansionMapIter I = MacroExpansions.begin(),
                               E = MacroExpansions.end();
         I != E; ++I) {
      const PPItemKey &ItemKey = I->first;
      MacroExpansionTracker &MacroExpTracker = I->second;
      // If no mismatch (only one instance value) continue.
      if (!MacroExpTracker.hasMismatch())
        continue;
      // Tell caller we found one or more errors.
      ReturnValue = true;
      // Start the error message.
      OS << *MacroExpTracker.InstanceSourceLine;
      if (ItemKey.Column > 0)
        OS << std::string(ItemKey.Column - 1, ' ') << "^\n";
      OS << "error: Macro instance '" << *MacroExpTracker.MacroUnexpanded
         << "' has different values in this header, depending on how it was "
            "included.\n";
      // Walk all the instances.
      for (std::vector<MacroExpansionInstance>::iterator
               IMT = MacroExpTracker.MacroExpansionInstances.begin(),
               EMT = MacroExpTracker.MacroExpansionInstances.end();
           IMT != EMT; ++IMT) {
        MacroExpansionInstance &MacroInfo = *IMT;
        OS << "  '" << *MacroExpTracker.MacroUnexpanded << "' expanded to: '"
           << *MacroInfo.MacroExpanded
           << "' with respect to these inclusion paths:\n";
        // Walk all the inclusion path hierarchies.
        for (std::vector<InclusionPathHandle>::iterator
                 IIP = MacroInfo.InclusionPathHandles.begin(),
                 EIP = MacroInfo.InclusionPathHandles.end();
             IIP != EIP; ++IIP) {
          const std::vector<HeaderHandle> &ip = getInclusionPath(*IIP);
          int Count = (int)ip.size();
          for (int Index = 0; Index < Count; ++Index) {
            HeaderHandle H = ip[Index];
            OS << std::string((Index * 2) + 4, ' ') << *getHeaderFilePath(H)
               << "\n";
          }
        }
        // For a macro that wasn't defined, we flag it by using the
        // instance location.
        // If there is a definition...
        if (MacroInfo.DefinitionLocation.Line != ItemKey.Line) {
          OS << *MacroInfo.DefinitionSourceLine;
          if (MacroInfo.DefinitionLocation.Column > 0)
            OS << std::string(MacroInfo.DefinitionLocation.Column - 1, ' ')
               << "^\n";
          OS << "Macro defined here.\n";
        } else
          OS << "(no macro definition)"
             << "\n";
      }
    }
    return ReturnValue;
  }

  // Report on inconsistent conditional instances.
  // Returns true if any mismatches.
  bool reportInconsistentConditionals(llvm::raw_ostream &OS) {
    bool ReturnValue = false;
    // Walk all the conditional trackers in the map.
    for (ConditionalExpansionMapIter I = ConditionalExpansions.begin(),
                                     E = ConditionalExpansions.end();
         I != E; ++I) {
      const PPItemKey &ItemKey = I->first;
      ConditionalTracker &CondTracker = I->second;
      if (!CondTracker.hasMismatch())
        continue;
      // Tell caller we found one or more errors.
      ReturnValue = true;
      // Start the error message.
      OS << *HeaderPaths[ItemKey.File] << ":" << ItemKey.Line << ":"
         << ItemKey.Column << "\n";
      OS << "#" << getDirectiveSpelling(CondTracker.DirectiveKind) << " "
         << *CondTracker.ConditionUnexpanded << "\n";
      OS << "^\n";
      OS << "error: Conditional expression instance '"
         << *CondTracker.ConditionUnexpanded
         << "' has different values in this header, depending on how it was "
            "included.\n";
      // Walk all the instances.
      for (std::vector<ConditionalExpansionInstance>::iterator
               IMT = CondTracker.ConditionalExpansionInstances.begin(),
               EMT = CondTracker.ConditionalExpansionInstances.end();
           IMT != EMT; ++IMT) {
        ConditionalExpansionInstance &MacroInfo = *IMT;
        OS << "  '" << *CondTracker.ConditionUnexpanded << "' expanded to: '"
           << (MacroInfo.ConditionValue ? "true" : "false")
           << "' with respect to these inclusion paths:\n";
        // Walk all the inclusion path hierarchies.
        for (std::vector<InclusionPathHandle>::iterator
                 IIP = MacroInfo.InclusionPathHandles.begin(),
                 EIP = MacroInfo.InclusionPathHandles.end();
             IIP != EIP; ++IIP) {
          const std::vector<HeaderHandle> &ip = getInclusionPath(*IIP);
          int Count = (int)ip.size();
          for (int Index = 0; Index < Count; ++Index) {
            HeaderHandle H = ip[Index];
            OS << std::string((Index * 2) + 4, ' ') << *getHeaderFilePath(H)
               << "\n";
          }
        }
      }
    }
    return ReturnValue;
  }

  // Get directive spelling.
  static const char *getDirectiveSpelling(clang::tok::PPKeywordKind kind) {
    switch (kind) {
    case clang::tok::pp_if:
      return "if";
    case clang::tok::pp_elif:
      return "elif";
    case clang::tok::pp_ifdef:
      return "ifdef";
    case clang::tok::pp_ifndef:
      return "ifndef";
    default:
      return "(unknown)";
    }
  }

private:
  llvm::StringPool Strings;
  std::vector<StringHandle> HeaderPaths;
  std::vector<HeaderHandle> HeaderStack;
  std::vector<HeaderInclusionPath> InclusionPaths;
  InclusionPathHandle CurrentInclusionPathHandle;
  MacroExpansionMap MacroExpansions;
  ConditionalExpansionMap ConditionalExpansions;
};

// PreprocessorTracker functions.

// PreprocessorTracker desctructor.
PreprocessorTracker::~PreprocessorTracker() {}

// Create instance of PreprocessorTracker.
PreprocessorTracker *PreprocessorTracker::create() {
  return new PreprocessorTrackerImpl();
}

// Preprocessor callbacks for modularize.

// Handle file entry/exit.
void PreprocessorCallbacks::FileChanged(
    clang::SourceLocation Loc, clang::PPCallbacks::FileChangeReason Reason,
    clang::SrcMgr::CharacteristicKind FileType, clang::FileID PrevFID) {
  switch (Reason) {
  case EnterFile:
    PPTracker.handleHeaderEntry(PP, getSourceLocationFile(PP, Loc));
    break;
  case ExitFile:
    if (PrevFID.isInvalid())
      PPTracker.handleHeaderExit(RootHeaderFile);
    else
      PPTracker.handleHeaderExit(getSourceLocationFile(PP, Loc));
    break;
  case SystemHeaderPragma:
  case RenameFile:
    break;
  }
}

// Handle macro expansion.
void PreprocessorCallbacks::MacroExpands(const clang::Token &MacroNameTok,
                                         const clang::MacroDirective *MD,
                                         clang::SourceRange Range,
                                         const clang::MacroArgs *Args) {
  clang::SourceLocation Loc = Range.getBegin();
  clang::IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  const clang::MacroInfo *MI = PP.getMacroInfo(II);
  std::string MacroName = II->getName().str();
  std::string Unexpanded(getMacroUnexpandedString(Range, PP, MacroName, MI));
  std::string Expanded(getMacroExpandedString(PP, MacroName, MI, Args));
  PPTracker.addMacroExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc, MI->getDefinitionLoc(), II,
      Unexpanded, Expanded, PPTracker.getCurrentInclusionPathHandle());
}

void PreprocessorCallbacks::Defined(const clang::Token &MacroNameTok,
                                    const clang::MacroDirective *MD,
                                    clang::SourceRange Range) {
  clang::SourceLocation Loc(Range.getBegin());
  clang::IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
  const clang::MacroInfo *MI = PP.getMacroInfo(II);
  std::string MacroName = II->getName().str();
  std::string Unexpanded(getSourceString(PP, Range));
  PPTracker.addMacroExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc,
      (MI ? MI->getDefinitionLoc() : Loc), II, Unexpanded,
      (MI ? "true" : "false"), PPTracker.getCurrentInclusionPathHandle());
}

void PreprocessorCallbacks::If(clang::SourceLocation Loc,
                               clang::SourceRange ConditionRange,
                               bool ConditionResult) {
  std::string Unexpanded(getSourceString(PP, ConditionRange));
  PPTracker.addConditionalExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc, clang::tok::pp_if,
      ConditionResult, Unexpanded, PPTracker.getCurrentInclusionPathHandle());
}

void PreprocessorCallbacks::Elif(clang::SourceLocation Loc,
                                 clang::SourceRange ConditionRange,
                                 bool ConditionResult,
                                 clang::SourceLocation IfLoc) {
  std::string Unexpanded(getSourceString(PP, ConditionRange));
  PPTracker.addConditionalExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc, clang::tok::pp_elif,
      ConditionResult, Unexpanded, PPTracker.getCurrentInclusionPathHandle());
}

void PreprocessorCallbacks::Ifdef(clang::SourceLocation Loc,
                                  const clang::Token &MacroNameTok,
                                  const clang::MacroDirective *MD) {
  bool IsDefined = (MD != 0);
  PPTracker.addConditionalExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc, clang::tok::pp_ifdef,
      IsDefined, PP.getSpelling(MacroNameTok),
      PPTracker.getCurrentInclusionPathHandle());
}

void PreprocessorCallbacks::Ifndef(clang::SourceLocation Loc,
                                   const clang::Token &MacroNameTok,
                                   const clang::MacroDirective *MD) {
  bool IsNotDefined = (MD == 0);
  PPTracker.addConditionalExpansionInstance(
      PP, PPTracker.getCurrentHeaderHandle(), Loc, clang::tok::pp_ifndef,
      IsNotDefined, PP.getSpelling(MacroNameTok),
      PPTracker.getCurrentInclusionPathHandle());
}
} // end namespace Modularize
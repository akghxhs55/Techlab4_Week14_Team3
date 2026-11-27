// KurisuHeaderTool (KHT)
// 사용법: KurisuHeaderTool <모듈 이름> <출력 폴더> <소스 폴더>... [--ref <참조 소스 폴더>...]
//   예:   KurisuHeaderTool Editor Build/Generated/Editor Source/Editor --ref Source/Runtime
//
// 소스 폴더의 헤더 중 UCLASS(), USTRUCT(), UENUM()이 있는 것마다 <출력 폴더>/<헤더 이름>.generated.h를 만든다.
// RegisterProperties 정의는 <헤더 이름>.gen.cpp에 만들고, <모듈 이름>.gen.cpp가 그것들을 모은다.
// --ref 뒤의 폴더는 파싱만 한다.
// 오류가 하나라도 있으면 아무 파일도 쓰지 않고 1을 반환해서 빌드를 멈춘다.

#include "CodeGenerator.h"
#include "HeaderParser.h"
#include "Tokenizer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace
{
    // Runtime/Components/BoxComponent.h → Runtime_Components_BoxComponent_h
    std::string MakeIdentifier(const std::string& Text)
    {
        std::string Result = Text;
        for (char& C : Result)
        {
            if (!std::isalnum((unsigned char)C))
            {
                C = '_';
            }
        }
        return Result;
    }

	// .generated.h는 UCLASS/USTRUCT/UENUM보다 앞에 있는 #include 중 마지막이어야 한다.
    // 뒤에 다른 헤더가 오면 그 헤더의 generated.h가 CURRENT_FILE_ID를 덮어쓴다.
    void CheckGeneratedInclude(const std::string& Source, const FHeaderInfo& Header, FDiagnostics& Diag)
    {
        const int FirstClassLine = Header.GetFirstDeclarationLine();
        const std::string Expected = "\"" + Header.GeneratedName + "\"";

        int GeneratedLine = 0;
        int LastIncludeLine = 0;

        std::istringstream Stream(Source);
        std::string LineText;
        for (int Line = 1; std::getline(Stream, LineText); ++Line)
        {
            const size_t First = LineText.find_first_not_of(" \t");
            if (First == std::string::npos || LineText[First] != '#')
            {
                continue;
            }
            const size_t Directive = LineText.find_first_not_of(" \t", First + 1);
            if (Directive == std::string::npos || LineText.compare(Directive, 7, "include") != 0)
            {
                continue;
            }

            if (LineText.find(Expected) != std::string::npos)
            {
                GeneratedLine = Line;
            }
            else if (Line < FirstClassLine)
            {
                LastIncludeLine = Line;
            }
        }

        if (GeneratedLine == 0)
        {
            Diag.Error(Header.Path, FirstClassLine, "#include " + Expected + " is missing. Add it as the last #include of the header.");
        }
        else if (GeneratedLine > FirstClassLine)
        {
            Diag.Error(Header.Path, GeneratedLine, Header.GeneratedName + " must be included before UCLASS()/USTRUCT()/UENUM().");
        }
        else if (LastIncludeLine > GeneratedLine)
        {
            Diag.Error(Header.Path, LastIncludeLine, Header.GeneratedName + " must be the last #include. Move this #include above it.");
        }
    }

    fs::path NormalizeDirectory(const fs::path& Directory)
    {
        fs::path Result = fs::absolute(Directory).lexically_normal();
        if (Result.filename().empty())
        {
            Result = Result.parent_path(); // 끝의 / 제거
        }
        return Result;
    }

    // 상대 경로라 한글 경로·subst 드라이브에서도 동작한다.
    std::string MakeIncludePath(const fs::path& Path, const fs::path& OutputDir)
    {
        fs::path Relative = fs::relative(Path, OutputDir);
        if (Relative.empty())
        {
            Relative = fs::absolute(Path).lexically_normal(); // 드라이브가 달라 상대 경로가 없을 때
        }
        std::string Result = ToUtf8(Relative);
        std::replace(Result.begin(), Result.end(), '\\', '/');
        return Result;
    }

    void CollectHeaders(const fs::path& SourceDir, std::vector<FHeaderInfo>& OutHeaders, FDiagnostics& Diag, bool bReference)
    {
        if (!fs::is_directory(SourceDir))
        {
            Diag.Error(SourceDir, 1, "Source Dir not exists.");
            return;
        }

        for (const fs::directory_entry& Entry : fs::recursive_directory_iterator(SourceDir))
        {
            const fs::path& Path = Entry.path();
            if (!Entry.is_regular_file() || (Path.extension() != ".h" && Path.extension() != ".hpp"))
            {
                continue;
            }

            std::string Source;
            if (!ReadTextFile(Path, Source))
            {
                Diag.Error(Path, 1, "Failed to read file.");
                continue;
            }
            // 대부분의 헤더는 여기서 걸러진다.
			if (Source.find("UCLASS") == std::string::npos && Source.find("USTRUCT") == std::string::npos && Source.find("UENUM") == std::string::npos)
            {
                continue;
            }

            std::vector<FToken> Tokens;
            if (!Tokenize(Source, Path, Tokens, Diag))
            {
                continue;
            }

            FHeaderInfo Header;
            Header.Path = Path;
            FHeaderParser Parser(Tokens, Path, Diag);
            if (!Parser.Parse(Header.Classes, Header.Enums) || (Header.Classes.empty() && Header.Enums.empty()))
            {
                continue;
            }

            // FileId는 소스 폴더의 부모 기준 경로로 만든다. 모듈끼리 겹치지 않게 Runtime/, Editor/가 앞에 붙는다.
            Header.RelativePath = fs::relative(Path, SourceDir.parent_path()).generic_string();
            Header.FileId = "FID_" + MakeIdentifier(Header.RelativePath);
            Header.GeneratedName = ToUtf8(Path.stem()) + ".generated.h";
            Header.SourceName = ToUtf8(Path.stem()) + ".gen.cpp";

            if (!bReference)
            {
                CheckGeneratedInclude(Source, Header, Diag);
            }
            OutHeaders.push_back(std::move(Header));
        }
    }

    int Run(int Argc, char** Argv)
    {
        const std::string Usage = "How To Use: KurisuHeaderTool <Module Name> <Output Folder> <Source Folder>... [--ref <Reference Source Folder>...]";
        if (Argc < 4 || std::string(Argv[3]) == "--ref")
        {
            PrintLine(Usage, true);
            return 2;
        }

        const std::string ModuleName = Argv[1];
        const fs::path OutputDir = NormalizeDirectory(Argv[2]);
        const std::string ModuleSourceName = ModuleName + ".gen.cpp";

        FDiagnostics Diag;
        std::vector<FHeaderInfo> Headers;
        std::vector<FHeaderInfo> ReferenceHeaders;
        bool bReference = false;
        for (int I = 3; I < Argc; ++I)
        {
            if (std::string(Argv[I]) == "--ref")
            {
                bReference = true;
                continue;
            }
            CollectHeaders(NormalizeDirectory(Argv[I]), bReference ? ReferenceHeaders : Headers, Diag, bReference);
        }

        for (FHeaderInfo& Header : Headers)
        {
            Header.IncludePath = MakeIncludePath(Header.Path, OutputDir);
        }
        for (FHeaderInfo& Header : ReferenceHeaders)
        {
            Header.IncludePath = MakeIncludePath(Header.Path, OutputDir);
        }

        // 생성 파일은 한 폴더에 모이므로 헤더 이름이 겹치면 안 된다.
        std::map<std::string, fs::path> GeneratedNames;
        for (const FHeaderInfo& Header : Headers)
        {
            const auto [It, bInserted] = GeneratedNames.emplace(Header.GeneratedName, Header.Path);
            if (!bInserted)
            {
                Diag.Error(Header.Path, 1, Header.GeneratedName + " Name " + ToUtf8(It->second) + " already exists. UCLASS/USTRUCT/UENUM header names must be unique within a module.");
            }
            if (Header.SourceName == ModuleSourceName)
            {
                Diag.Error(Header.Path, 1, Header.SourceName + " is reserved for the module source. Rename the header.");
            }
        }

        std::map<std::string, const FHeaderInfo*> TypeToHeader;
        for (const std::vector<FHeaderInfo>* List : { &Headers, &ReferenceHeaders })
        {
            for (const FHeaderInfo& Header : *List)
            {
                for (const FClassInfo& Class : Header.Classes)
                {
                    TypeToHeader.emplace(Class.Name, &Header);
                }
            }
        }

        if (Diag.GetErrorCount() > 0)
        {
            PrintLine("KHT: " + ModuleName + " - " + std::to_string(Diag.GetErrorCount()) + "Errors. Not Written Generated Files.", true);
            return 1;
        }

        fs::create_directories(OutputDir);

        int WrittenCount = 0;
        std::set<std::string> SourceNames;
        for (const FHeaderInfo& Header : Headers)
        {
            bool bWritten = false;
            if (!WriteIfChanged(OutputDir / Header.GeneratedName, GenerateHeader(Header, ModuleName), bWritten, Diag))
            {
                return 1;
            }
            WrittenCount += bWritten ? 1 : 0;

            if (HasGeneratedSource(Header))
            {
                if (!WriteIfChanged(OutputDir / Header.SourceName, GenerateSource(Header, TypeToHeader), bWritten, Diag))
                {
                    return 1;
                }
                WrittenCount += bWritten ? 1 : 0;
                SourceNames.insert(Header.SourceName);
            }
        }

        // 프로젝트가 이 파일을 찾으므로 .gen.cpp가 하나도 없어도 만든다.
        bool bModuleWritten = false;
        if (!WriteIfChanged(OutputDir / ModuleSourceName, GenerateModuleSource(ModuleName, SourceNames), bModuleWritten, Diag))
        {
            return 1;
        }
        WrittenCount += bModuleWritten ? 1 : 0;
        const size_t FileCount = Headers.size() + SourceNames.size() + 1;

		// UCLASS/USTRUCT/UENUM이 사라진 헤더의 생성 파일은 지운다.
        int RemovedCount = 0;
        for (const fs::directory_entry& Entry : fs::directory_iterator(OutputDir))
        {
            const std::string Name = ToUtf8(Entry.path().filename());
            const bool bStaleHeader = Name.ends_with(".generated.h") && !GeneratedNames.contains(Name);
            const bool bStaleSource = Name.ends_with(".gen.cpp") && Name != ModuleSourceName && !SourceNames.contains(Name);
            if (bStaleHeader || bStaleSource)
            {
                fs::remove(Entry.path());
                ++RemovedCount;
            }
        }

        PrintLine("KHT: " + ModuleName + " - Reflection Header " + std::to_string(Headers.size()) + ", Source " + std::to_string(SourceNames.size())
            + " (Written " + std::to_string(WrittenCount) + ", Not Written " + std::to_string(FileCount - WrittenCount)
            + ", Removed " + std::to_string(RemovedCount) + ")");
        return 0;
    }
}

int main(int Argc, char** Argv)
{
    try
    {
        return Run(Argc, Argv);
    }
    catch (const fs::filesystem_error& Exception)
    {
        PrintLine(std::string("KHT: File System Error: ") + Exception.what(), true);
        return 1;
    }
}
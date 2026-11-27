#pragma once

#include "Diagnostics.h"

#include <map>
#include <set>

// FHeaderInfo 하나를 .generated.h 텍스트로 만든다.
std::string GenerateHeader(const FHeaderInfo& Header, const std::string& ModuleName);

bool HasGeneratedSource(const FHeaderInfo& Header);

// TypeToHeader: UCLASS/USTRUCT 이름 → 선언한 헤더 (참조 모듈 포함)
std::string GenerateSource(const FHeaderInfo& Header, const std::map<std::string, const FHeaderInfo*>& TypeToHeader);

std::string GenerateModuleSource(const std::string& ModuleName, const std::set<std::string>& SourceNames);

// 기존 파일과 내용이 같으면 쓰지 않는다. 파일 시간이 그대로여야 VS가 다시 컴파일하지 않는다.
// 썼으면 bOutWritten = true. 쓰기에 실패하면 false
bool WriteIfChanged(const fs::path& Path, const std::string& Content, bool& bOutWritten, FDiagnostics& Diag);

bool ReadTextFile(const fs::path& Path, std::string& OutText);

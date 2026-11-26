#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

// 에디터 드롭다운 목록을 알파벳순으로 정렬하는 도우미
namespace EditorSort
{
	inline bool LessIgnoreCase(const std::string& A, const std::string& B)
	{
		const size_t Count = std::min(A.size(), B.size());
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const int CharA = std::tolower(static_cast<unsigned char>(A[Index]));
			const int CharB = std::tolower(static_cast<unsigned char>(B[Index]));
			if (CharA != CharB)
				return CharA < CharB;
		}
		if (A.size() != B.size())
			return A.size() < B.size();
		return A < B;
	}

	template <typename NameGetterType>
	struct TNameLessComparator
	{
		NameGetterType& GetName;

		template <typename ElemType>
		bool operator()(const ElemType& A, const ElemType& B) const
		{
			return LessIgnoreCase(GetName(A), GetName(B));
		}
	};

	template <typename ContainerType, typename NameGetterType>
	void SortByName(ContainerType& Items, NameGetterType&& GetName)
	{
		TNameLessComparator<NameGetterType> Comparator{ GetName };
		std::stable_sort(Items.begin(), Items.end(), Comparator);
	}

	template <typename ObjectType, typename IteratorType, typename NameGetterType, typename FilterType>
	TArray<ObjectType*> CollectSorted(IteratorType It, NameGetterType&& GetName, FilterType&& Filter)
	{
		TArray<ObjectType*> Result;
		for (; It; ++It)
		{
			ObjectType* Object = *It;
			if (Object && Filter(Object))
				Result.Add(Object);
		}
		SortByName(Result, GetName);
		return Result;
	}
}

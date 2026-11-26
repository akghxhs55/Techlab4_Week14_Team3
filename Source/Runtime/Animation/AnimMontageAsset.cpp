#include "EnginePCH.h"
#include "Animation/AnimMontageAsset.h"
#include "Asset/AssetUtils.h"
#include "json.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace
{
	bool ReadFloatField(const json& Object, const char* Key, float& Out)
	{
		if (!Object.contains(Key) || !Object[Key].is_number())
			return false;
		Out = Object[Key].get<float>();
		return std::isfinite(Out);
	}

	bool ReadIntField(const json& Object, const char* Key, int32& Out)
	{
		if (!Object.contains(Key) || !Object[Key].is_number_integer())
			return false;
		Out = Object[Key].get<int32>();
		return true;
	}

	fs::path ResolveAssetFile(const FString& Path, const char* Ext)
	{
		if (Path.empty())
			return {};
		const FString Resolved = FAssetUtils::ResolveAssetFilePath(Path);
		fs::path FilePath = Resolved.empty() ? fs::path(Path) : fs::path(Resolved);
		if (FilePath.extension() != Ext)
			return {};
		return FilePath;
	}

	bool WriteJsonAtomically(const fs::path& File, const json& Data, FString& OutError)
	{
		std::error_code Ec;
		fs::create_directories(File.parent_path(), Ec);
		if (Ec)
		{
			OutError = "Cannot create directory.";
			return false;
		}
		const fs::path Temp = File.string() + ".editing";
		std::ofstream Out(Temp, std::ios::trunc);
		if (!Out)
		{
			OutError = "Cannot write file.";
			return false;
		}
		Out << Data.dump(2);
		Out.close();
		if (!Out)
		{
			OutError = "Cannot finish writing file.";
			fs::remove(Temp, Ec);
			return false;
		}
		if (!MoveFileExW(Temp.c_str(), File.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			OutError = "Cannot replace file.";
			fs::remove(Temp, Ec);
			return false;
		}
		OutError.clear();
		return true;
	}
}

bool FAnimMontageAsset::Validate(FString& OutError) const
{
	if (SlotName.empty())
	{
		OutError = "Slot name is empty.";
		return false;
	}
	if (!std::isfinite(PlayRate) || PlayRate <= 0.0f)
	{
		OutError = "PlayRate must be positive.";
		return false;
	}
	if (!std::isfinite(BlendInTime) || BlendInTime < 0.0f ||
		!std::isfinite(BlendOutTime) || BlendOutTime < 0.0f)
	{
		OutError = "Invalid blend time.";
		return false;
	}
	for (const FAnimMontageSegment& Seg : Segments)
	{
		if (Seg.SequencePath.empty() || !std::isfinite(Seg.StartTime) || !std::isfinite(Seg.EndTime) ||
			Seg.StartTime < 0.0f || Seg.EndTime < Seg.StartTime ||
			!std::isfinite(Seg.AnimStartOffset) || Seg.AnimStartOffset < 0.0f ||
			!std::isfinite(Seg.PlayRate) || Seg.PlayRate <= 0.0f)
		{
			OutError = "Invalid montage segment.";
			return false;
		}
	}
	std::set<FString> SectionNames;
	for (const FAnimMontageSection& Sec : Sections)
	{
		if (Sec.Name.empty() || !SectionNames.insert(Sec.Name).second ||
			!std::isfinite(Sec.StartTime) || !std::isfinite(Sec.EndTime) ||
			Sec.StartTime < 0.0f || Sec.EndTime <= Sec.StartTime)
		{
			OutError = "Invalid montage section.";
			return false;
		}
	}
	for (const FAnimMontageNotify& Notify : Notifies)
	{
		if (Notify.Name.empty() || !std::isfinite(Notify.Time) || Notify.Time < 0.0f ||
			!std::isfinite(Notify.Duration) || Notify.Duration < 0.0f)
		{
			OutError = "Invalid montage notify.";
			return false;
		}
	}
	OutError.clear();
	return true;
}

bool FAnimMontageAsset::Load(const FString& Path, FAnimMontageAsset& Out, FString& OutError)
{
	const fs::path File = ResolveAssetFile(Path, ".animmontage");
	if (File.empty())
	{
		OutError = "Invalid AnimMontage path.";
		return false;
	}
	std::ifstream In(File);
	if (!In)
	{
		OutError = "Cannot open AnimMontage.";
		return false;
	}
	json Data = json::parse(In, nullptr, false);
	if (!Data.is_object() || !Data.contains("Version") || Data["Version"] != 1)
	{
		OutError = "Invalid AnimMontage format.";
		return false;
	}

	FAnimMontageAsset Loaded;
	if (Data.contains("PreviewMesh") && Data["PreviewMesh"].is_string())
		Loaded.PreviewMeshPath = Data["PreviewMesh"].get<FString>();
	else if (Data.contains("Skeleton") && Data["Skeleton"].is_string())
		Loaded.PreviewMeshPath = Data["Skeleton"].get<FString>();
	if (Data.contains("Sequence") && Data["Sequence"].is_string())
		Loaded.SequencePath = Data["Sequence"].get<FString>();
	if (Data.contains("Slot") && Data["Slot"].is_string())
		Loaded.SlotName = Data["Slot"].get<FString>();
	if (Data.contains("PlayRate") && !ReadFloatField(Data, "PlayRate", Loaded.PlayRate))
	{
		OutError = "Invalid PlayRate.";
		return false;
	}
	if (Data.contains("BlendInTime") && !ReadFloatField(Data, "BlendInTime", Loaded.BlendInTime))
	{
		OutError = "Invalid BlendInTime.";
		return false;
	}
	if (Data.contains("BlendOutTime") && !ReadFloatField(Data, "BlendOutTime", Loaded.BlendOutTime))
	{
		OutError = "Invalid BlendOutTime.";
		return false;
	}
	if (Data.contains("Loop") && Data["Loop"].is_boolean())
		Loaded.bLoop = Data["Loop"].get<bool>();

	if (Data.contains("Segments") && Data["Segments"].is_array())
	{
		for (const json& Item : Data["Segments"])
		{
			FAnimMontageSegment Seg;
			if (!Item.is_object() || !Item.contains("Sequence") || !Item["Sequence"].is_string() ||
				!ReadFloatField(Item, "StartTime", Seg.StartTime) || !ReadFloatField(Item, "EndTime", Seg.EndTime))
			{
				OutError = "Invalid segment entry.";
				return false;
			}
			Seg.SequencePath = Item["Sequence"].get<FString>();
			if (Item.contains("AnimStartOffset") && !ReadFloatField(Item, "AnimStartOffset", Seg.AnimStartOffset))
			{
				OutError = "Invalid segment offset.";
				return false;
			}
			if (Item.contains("PlayRate") && !ReadFloatField(Item, "PlayRate", Seg.PlayRate))
			{
				OutError = "Invalid segment play rate.";
				return false;
			}
			Loaded.Segments.Add(std::move(Seg));
		}
		if (Loaded.SequencePath.empty() && !Loaded.Segments.IsEmpty())
			Loaded.SequencePath = Loaded.Segments[0].SequencePath;
	}

	if (Data.contains("Sections") && Data["Sections"].is_array())
	{
		for (const json& Item : Data["Sections"])
		{
			FAnimMontageSection Sec;
			if (!Item.is_object() || !Item.contains("Name") || !Item["Name"].is_string() ||
				!ReadFloatField(Item, "StartTime", Sec.StartTime) || !ReadFloatField(Item, "EndTime", Sec.EndTime))
			{
				OutError = "Invalid section entry.";
				return false;
			}
			Sec.Name = Item["Name"].get<FString>();
			if (Item.contains("NextSection") && Item["NextSection"].is_string())
				Sec.NextSection = Item["NextSection"].get<FString>();
			Loaded.Sections.Add(std::move(Sec));
		}
	}

	if (Data.contains("Notifies") && Data["Notifies"].is_array())
	{
		for (const json& Item : Data["Notifies"])
		{
			FAnimMontageNotify Notify;
			if (!Item.is_object() || !Item.contains("Name") || !Item["Name"].is_string() ||
				!ReadFloatField(Item, "Time", Notify.Time))
			{
				OutError = "Invalid notify entry.";
				return false;
			}
			Notify.Name = Item["Name"].get<FString>();
			if (Item.contains("Duration") && !ReadFloatField(Item, "Duration", Notify.Duration))
			{
				OutError = "Invalid notify duration.";
				return false;
			}
			if (Item.contains("ClassName") && Item["ClassName"].is_string())
				Notify.ClassName = Item["ClassName"].get<FString>();
			if (Item.contains("Properties"))
			{
				if (Item["Properties"].is_object())
					Notify.Properties = Item["Properties"].dump(-1, ' ', false, json::error_handler_t::replace);
				else if (Item["Properties"].is_string())
					Notify.Properties = Item["Properties"].get<FString>();
			}
			Loaded.Notifies.Add(std::move(Notify));
		}
	}

	if (!Loaded.Validate(OutError))
		return false;
	Out = std::move(Loaded);
	return true;
}

bool FAnimMontageAsset::Save(const FString& Path, FString& OutError) const
{
	if (!Validate(OutError))
		return false;
	const fs::path File = ResolveAssetFile(Path, ".animmontage");
	if (File.empty())
	{
		OutError = "Invalid AnimMontage path.";
		return false;
	}
	const FString PrimarySequence = !Segments.IsEmpty() ? Segments[0].SequencePath : SequencePath;
	json Data = {
		{ "Version", 1 },
		{ "PreviewMesh", PreviewMeshPath },
		{ "Sequence", PrimarySequence },
		{ "Slot", SlotName },
		{ "PlayRate", PlayRate },
		{ "BlendInTime", BlendInTime },
		{ "BlendOutTime", BlendOutTime },
		{ "Loop", bLoop },
		{ "Segments", json::array() },
		{ "Sections", json::array() },
		{ "Notifies", json::array() }
	};
	for (const FAnimMontageSegment& Seg : Segments)
	{
		Data["Segments"].push_back({
			{ "Sequence", Seg.SequencePath },
			{ "StartTime", Seg.StartTime },
			{ "EndTime", Seg.EndTime },
			{ "AnimStartOffset", Seg.AnimStartOffset },
			{ "PlayRate", Seg.PlayRate }
		});
	}
	for (const FAnimMontageSection& Sec : Sections)
	{
		Data["Sections"].push_back({
			{ "Name", Sec.Name },
			{ "StartTime", Sec.StartTime },
			{ "EndTime", Sec.EndTime },
			{ "NextSection", Sec.NextSection }
		});
	}
	for (const FAnimMontageNotify& Notify : Notifies)
	{
		json Entry = {
			{ "Name", Notify.Name },
			{ "Time", Notify.Time }
		};
		if (Notify.Duration > 0.0f)
			Entry["Duration"] = Notify.Duration;
		if (!Notify.ClassName.empty())
			Entry["ClassName"] = Notify.ClassName;
		if (!Notify.Properties.empty())
		{
			json Parsed = json::parse(Notify.Properties, nullptr, false);
			Entry["Properties"] = Parsed.is_object() ? Parsed : json(Notify.Properties);
		}
		Data["Notifies"].push_back(std::move(Entry));
	}
	return WriteJsonAtomically(File, Data, OutError);
}

bool FAimOffsetAsset::Validate(FString& OutError) const
{
	if (!std::isfinite(MinPitch) || !std::isfinite(MaxPitch) || MinPitch >= MaxPitch)
	{
		OutError = "Invalid Pitch range.";
		return false;
	}
	for (const FAimOffsetSample& Sample : Samples)
	{
		if (!std::isfinite(Sample.Pitch) || Sample.FrameIndex < 0)
		{
			OutError = "Invalid AimOffset sample.";
			return false;
		}
	}
	OutError.clear();
	return true;
}

bool FAimOffsetAsset::Load(const FString& Path, FAimOffsetAsset& Out, FString& OutError)
{
	const fs::path File = ResolveAssetFile(Path, ".aimoffset");
	if (File.empty())
	{
		OutError = "Invalid AimOffset path.";
		return false;
	}
	std::ifstream In(File);
	if (!In)
	{
		OutError = "Cannot open AimOffset.";
		return false;
	}
	json Data = json::parse(In, nullptr, false);
	if (!Data.is_object() || !Data.contains("Version") || Data["Version"] != 1)
	{
		OutError = "Invalid AimOffset format.";
		return false;
	}
	FAimOffsetAsset Loaded;
	if (Data.contains("PreviewMesh") && Data["PreviewMesh"].is_string())
		Loaded.PreviewMeshPath = Data["PreviewMesh"].get<FString>();
	else if (Data.contains("Skeleton") && Data["Skeleton"].is_string())
		Loaded.PreviewMeshPath = Data["Skeleton"].get<FString>();
	if (Data.contains("BaseSequence") && Data["BaseSequence"].is_string())
		Loaded.BaseSequencePath = Data["BaseSequence"].get<FString>();
	if (Data.contains("BaseFrameIndex"))
		ReadIntField(Data, "BaseFrameIndex", Loaded.BaseFrameIndex);
	if (Data.contains("MinPitch") && !ReadFloatField(Data, "MinPitch", Loaded.MinPitch))
	{
		OutError = "Invalid MinPitch.";
		return false;
	}
	if (Data.contains("MaxPitch") && !ReadFloatField(Data, "MaxPitch", Loaded.MaxPitch))
	{
		OutError = "Invalid MaxPitch.";
		return false;
	}
	if (Data.contains("Samples") && Data["Samples"].is_array())
	{
		for (const json& Item : Data["Samples"])
		{
			FAimOffsetSample Sample;
			if (!Item.is_object() || !ReadFloatField(Item, "Pitch", Sample.Pitch) ||
				!Item.contains("Sequence") || !Item["Sequence"].is_string())
			{
				OutError = "Invalid AimOffset sample.";
				return false;
			}
			Sample.SequencePath = Item["Sequence"].get<FString>();
			if (Item.contains("FrameIndex"))
				ReadIntField(Item, "FrameIndex", Sample.FrameIndex);
			Loaded.Samples.Add(std::move(Sample));
		}
	}
	if (!Loaded.Validate(OutError))
		return false;
	Out = std::move(Loaded);
	return true;
}

bool FAimOffsetAsset::Save(const FString& Path, FString& OutError) const
{
	if (!Validate(OutError))
		return false;
	const fs::path File = ResolveAssetFile(Path, ".aimoffset");
	if (File.empty())
	{
		OutError = "Invalid AimOffset path.";
		return false;
	}
	json Data = {
		{ "Version", 1 },
		{ "PreviewMesh", PreviewMeshPath },
		{ "BaseSequence", BaseSequencePath },
		{ "BaseFrameIndex", BaseFrameIndex },
		{ "MinPitch", MinPitch },
		{ "MaxPitch", MaxPitch },
		{ "Samples", json::array() }
	};
	for (const FAimOffsetSample& Sample : Samples)
	{
		Data["Samples"].push_back({
			{ "Pitch", Sample.Pitch },
			{ "Sequence", Sample.SequencePath },
			{ "FrameIndex", Sample.FrameIndex }
		});
	}
	return WriteJsonAtomically(File, Data, OutError);
}

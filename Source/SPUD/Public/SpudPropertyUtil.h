#pragma once
#include "CoreMinimal.h"
#include "SpudData.h"
#include "GameFramework/Actor.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpudProps, Log, All);
namespace{
/// Type info for persistence
/// Maps a given type to:
/// 1. An enum value, for describing how the data is stored
/// 2. A storage type, for *casting* the data before writing to ensure it conforms to 1.
/// The latter is useful mostly to make sure we have control over the size of bools and enums
template <typename T> struct SpudTypeInfo
{
	static const ESpudStorageType EnumType;
	using StorageType = T;
};
// Bool needs a special case so that StorageType is uint8 (bools can otherwise write as 32-bit values)
template <> struct SpudTypeInfo<bool>
{
	static const ESpudStorageType EnumType = ESST_UInt8;
	using StorageType = uint8;
};
// This is a placeholder for any enum value
// Special cased so we always write enums as uint16
struct SpudAnyEnum {};
template <> struct SpudTypeInfo<SpudAnyEnum>
{
	static const ESpudStorageType EnumType = ESST_UInt16;
	using StorageType = uint16;
};
// Actor references need a special case, stored as FStrings
template <> struct SpudTypeInfo<AActor*>
{
	static const ESpudStorageType EnumType = ESST_String;
	using StorageType = FString;
};
// Nested UObjects are stored as a ClassID
template <> struct SpudTypeInfo<UObject*>
{
	static const ESpudStorageType EnumType = ESST_UInt32;
	using StorageType = uint32;
};
// TSubclassOfs are stored as a ClassID
template <> struct SpudTypeInfo<UClass*>
{
	static const ESpudStorageType EnumType = ESST_UInt32;
	using StorageType = uint32;
};
// Special case multicast delegates have a bit to them.
struct SpudMulticastDelegate {};
template <> struct SpudTypeInfo<SpudMulticastDelegate>
{
	static const ESpudStorageType EnumType = ESST_MulticastDelegate;
};
/// Now the simpler types where StorageType == input type 
template <> const ESpudStorageType SpudTypeInfo<uint8>::EnumType = ESST_UInt8;
template <> const ESpudStorageType SpudTypeInfo<uint16>::EnumType = ESST_UInt16;
template <> const ESpudStorageType SpudTypeInfo<uint32>::EnumType = ESST_UInt32;
template <> const ESpudStorageType SpudTypeInfo<uint64>::EnumType = ESST_UInt64;
template <> const ESpudStorageType SpudTypeInfo<int8>::EnumType = ESST_Int8;
template <> const ESpudStorageType SpudTypeInfo<int16>::EnumType = ESST_Int16;
template <> const ESpudStorageType SpudTypeInfo<int>::EnumType = ESST_Int32;
template <> const ESpudStorageType SpudTypeInfo<int64>::EnumType = ESST_Int64;
template <> const ESpudStorageType SpudTypeInfo<float>::EnumType = ESST_Float;
template <> const ESpudStorageType SpudTypeInfo<double>::EnumType = ESST_Double;
template <> const ESpudStorageType SpudTypeInfo<FVector>::EnumType = ESST_Vector;
template <> const ESpudStorageType SpudTypeInfo<FRotator>::EnumType = ESST_Rotator;
template <> const ESpudStorageType SpudTypeInfo<FTransform>::EnumType = ESST_Transform;
template <> const ESpudStorageType SpudTypeInfo<FGuid>::EnumType = ESST_Guid;
template <> const ESpudStorageType SpudTypeInfo<FString>::EnumType = ESST_String;
template <> const ESpudStorageType SpudTypeInfo<FName>::EnumType = ESST_Name;
template <> const ESpudStorageType SpudTypeInfo<FText>::EnumType = ESST_Text;

template<typename FieldType>
FORCEINLINE const FieldType* ExactCastConstField(const FField* Src)
{
	return (Src && (Src->GetClass() == FieldType::StaticClass())) ? static_cast<const FieldType*>(Src) : nullptr;
}
}
/// Utility class which does all the nuts & bolts related to property persistence without actually being stateful
/// Also none of this is exposed to Blueprints, is completely internal to C++ persistence
class SPUD_API SpudPropertyUtil
{
public:
	/**
	 * @brief The PropertyVisitor class is able to receive a predictable sequence of properties from a UObject, including
	 * nested struct properties.
	 */
	class PropertyVisitor
	{
	public:
		virtual ~PropertyVisitor() = default;
		
		/**
		 * @brief Visit a property and perform some action. For nested structs, this will be called for the struct
		 * itself and its nested properties.
		 * @param RootObject The root object for this property. Can  be null if just parsing definitions not instances!
		 * @param Property The property to process
		 * @param CurrentPrefixID The prefix which identifies nested struct properties
		 * @param ContainerPtr Pointer to data container which can be used to access values. Can be null!
		 * @param Depth The current nesting depth (0 for top-level properties, higher for nested structs)
		 * @returns True to continue parsing properties, false to quit early
		 */
		virtual bool VisitProperty(UObject* RootObject, FProperty* Property, uint32 CurrentPrefixID,
		                           void* ContainerPtr, int Depth) = 0;
		
		/**
		* @brief Be informed about an unsupported property. This is a property which is marked as persistent but
		* is not currently supported.
		* @param RootObject The root object for this property. Can  be null if just parsing definitions not instances!
		* @param Property The property to process
		* @param CurrentPrefixID The prefix which identifies nested struct properties
		* @param Depth The current nesting depth (0 for top-level properties, higher for nested structs)
		*/
		virtual void UnsupportedProperty(UObject* RootObject, FProperty* Property, uint32 CurrentPrefixID, int Depth) {}
		/**
		 * @brief Generate a nested prefix ID for properties underneath a struct or uobject property
		* @param Prop The property identifying the custom struct
		* @param CurrentPrefixID The current prefix up to this point
		* @return The new PrefixID for properties nested within this struct. If you return SPUD_PREFIXID_NONE then
		* nested properties will be skipped.
		 */
		virtual uint32 GetNestedPrefix(FProperty* Prop, uint32 CurrentPrefixID) = 0;

		/**
		 * Called just before descending into a struct 
		 * @param RootObject The root object being traversed
		 * @param SProp The property of the struct
		 * @param PrefixID The prefix ID for members of the struct
		 * @param Depth The depth for members of the struct
		 */
		virtual void StartNestedStruct(UObject* RootObject, FStructProperty* SProp, uint32 PrefixID, int Depth) {}
		/**
		 * Called just after all the members of a struct have been visited 
		 * @param RootObject The root object being traversed
		 * @param SProp The property of the struct
		 * @param PrefixID The prefix ID for members of the struct
		 * @param Depth The depth for members of the struct
		 */
		virtual void EndNestedStruct(UObject* RootObject, FStructProperty* SProp, uint32 PrefixID, int Depth) {}
	};

	/**
	 * @brief Return whether a specified property should be included in the persistent state of an object
	 * @param Property the property to potentially be included
	 * @param IsChildOfSaveGame whether this property is a child of another property which was marked as SaveGame
	 * @return Whether this property should be included in the persistent state
	 */
	static bool ShouldPropertyBeIncluded(FProperty* Property, bool IsChildOfSaveGame);	
	/**
	 * @brief Return wether a specified property is supported by the persistence system or not
	 * @param Property the property in question
	 * @return Whether this property is supported in the persistence system
	 */
	static bool IsPropertySupported(FProperty* Property);
	/**
	 * @brief Return whether a property is of a built-in struct 
	 * @param SProp The struct property
	 * @return Whether this property is of a built-in type (e.g. FVector)
	 */
	static bool IsBuiltInStructProperty(const FStructProperty* SProp);

	static bool IsCustomStructProperty(const FProperty* Property);

	/// Whether a property is an actor reference
	static bool IsActorObjectProperty(const FProperty* Property);
	/// Whether a property represents a nested UObject 
	static bool IsNestedUObjectProperty(const FProperty* Property);
	// Whether a property is a TSubclassOf property
	static bool IsSubclassOfProperty(const FProperty* Property);
	static uint16 GetPropertyDataType(const FProperty* Prop);

	class StoredMatchesRuntimePropertyVisitor : public SpudPropertyUtil::PropertyVisitor
	{
	protected:
		TArray<FSpudPropertyDef>::TConstIterator StoredPropertyIterator;
		const FSpudClassDef& ClassDef;
		const FSpudClassMetadata& Meta;
		bool bMatches;
	public:
		StoredMatchesRuntimePropertyVisitor(TArray<FSpudPropertyDef>::TConstIterator InStoredPropertyIterator,
                                            const FSpudClassDef& InClassDef, const FSpudClassMetadata& InMeta);
		virtual bool VisitProperty(UObject* RootObject, FProperty* Property, uint32 CurrentPrefixID,
		                           void* ContainerPtr, int Depth) override;
		virtual uint32 GetNestedPrefix(FProperty* Prop, uint32 CurrentPrefixID) override;
		// After visiting, was everything a match
		bool IsMatch() const { return bMatches; }
	};
	static bool StoredPropertyTypeMatchesRuntime(const FProperty* RuntimeProperty,
                                          const FSpudPropertyDef& StoredProperty,
                                          bool bIgnoreArrayFlag);

	
	static FString GetNestedPrefix(uint32 PrefixIDSoFar, FProperty* Prop, const FSpudClassMetadata& Meta);
	static uint32 GetNestedPrefixID(uint32 PrefixIDSoFar, FProperty* Prop, const FSpudClassMetadata& Meta);
	static uint32 FindOrAddNestedPrefixID(uint32 PrefixIDSoFar, FProperty* Prop, FSpudClassMetadata& Meta);
	static void RegisterProperty(uint32 PropNameID, uint32 PrefixID, uint16 DataType, TSharedPtr<FSpudClassDef> ClassDef, TPrefixedPropertyOffsets& PrefixToPropertyOffsets, FArchive& Out);
	static void RegisterProperty(const FString& Name,
	                             uint32 PrefixID,
	                             uint16 DataType,
	                             TSharedPtr<FSpudClassDef> ClassDef,
	                             TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                             FSpudClassMetadata& Meta,
	                             FArchive& Out);
	static void RegisterProperty(FProperty* Prop,
	                             uint32 PrefixID,
	                             TSharedPtr<FSpudClassDef> ClassDef,
	                             TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                             FSpudClassMetadata
	                             & Meta,
	                             FArchive& Out);

	/// Visit all properties of a UObject
	static void VisitPersistentProperties(UObject* RootObject, PropertyVisitor& Visitor, uint32 PrefixID, int StartDepth = 0);
	/// Visit all properties of a class definition, with no instance
	static void VisitPersistentProperties(const UStruct* Definition, PropertyVisitor& Visitor);

	/// World reference lookups used in store and restore
	/// The SPUD state prepares this structure for the calls.
	struct FWorldReferenceLookups
	{
		const TMap<FGuid, UObject*>* RuntimeObjectMap = nullptr;
		const TMap<FString, ULevel*>* WorldLevelsMap = nullptr;
		const TMap<TWeakObjectPtr<ULevel>, FString>* WorldLevelToNameMap = nullptr;
		const TMap<FString, FString>* PatchNamesMapping = nullptr;
	};
	
	static void StoreProperty(const UObject* RootObject,
	                          FProperty* Property,
	                          uint32 PrefixID,
	                          const void* ContainerPtr,
	                          int Depth,
	                          TSharedPtr<FSpudClassDef> ClassDef,
	                          TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
							  const FWorldReferenceLookups& WorldReferenceLookups,
	                          FSpudClassMetadata& Meta,
	                          FMemoryWriter& Out);
	static void StoreArrayProperty(FArrayProperty* AProp,
	                               const UObject* RootObject,
	                               uint32 PrefixID,
	                               const void* ContainerPtr,
	                               int Depth,
	                               TSharedPtr<FSpudClassDef> ClassDef,
	                               TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
								   const FWorldReferenceLookups& WorldReferenceLookups,
	                               FSpudClassMetadata& Meta,
	                               FMemoryWriter& Out);
	static void StoreContainerProperty(FProperty* Property,
	                                   const UObject* RootObject,
	                                   uint32 PrefixID,
	                                   const void* ContainerPtr,
	                                   bool bIsArrayElement,
	                                   int Depth,
	                                   TSharedPtr<FSpudClassDef> ClassDef,
	                                   TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
									   const FWorldReferenceLookups& WorldReferenceLookups,
	                                   FSpudClassMetadata& Meta,
	                                   FMemoryWriter& Out);


	
	static void RestoreProperty(UObject* RootObject, FProperty* Property, void* ContainerPtr,
	                            const FSpudPropertyDef& StoredProperty,
	                            const FWorldReferenceLookups& WorldReferenceLookups,
	                            const FSpudClassMetadata& Meta,
	                            int Depth, FMemoryReader& DataIn);
	static void RestoreArrayProperty(UObject* RootObject, FArrayProperty* const AProp, void* ContainerPtr,
	                                 const FSpudPropertyDef& StoredProperty,
	                                 const FWorldReferenceLookups& WorldReferenceLookups,
	                                 const FSpudClassMetadata& Meta,
	                                 int Depth, FMemoryReader& DataIn);
	static void RestoreContainerProperty(UObject* RootObject, FProperty* const Property,
	                                     void* ContainerPtr, const FSpudPropertyDef& StoredProperty,
	                                     const FWorldReferenceLookups& WorldReferenceLookups,
	                                     const FSpudClassMetadata& Meta,
	                                     int Depth, FMemoryReader& DataIn);


	/// Utility function for checking whether iterating through the properties on a UObject results in the same
	/// sequence of properties in a stored class definition (no saved game class changes since stored).
	/// If so, we can restore data much more efficiently because we don't have to look anything up on instances, just
	/// iterate through both sides.
	static bool StoredClassDefMatchesRuntime(const FSpudClassDef& ClassDef, const FSpudClassMetadata& Meta);

protected:
	static bool IsValidArrayType(FArrayProperty* AProp);
	/// General recursive visitation of properties, returns false to early-out, object/container can be null
	static bool VisitPersistentProperties(UObject* RootObject, const UStruct* Definition, uint32 PrefixID,
	                                      void* ContainerPtr, bool IsChildOfSaveGame, int Depth,
	                                      PropertyVisitor& Visitor);

	static FString ToString(int Val) { return FString::FromInt(Val); }
    static FString ToString(int64 Val) { return FString::Printf(TEXT("%lld"), Val); }
    static FString ToString(uint32 Val) { return FString::Printf(TEXT("%u"), Val); }
    static FString ToString(uint64 Val) { return FString::Printf(TEXT("%llu"), Val); }
    static FString ToString(bool Val) { return Val ? "True" : "False"; }
	static FString ToString(float Val) { return FString::SanitizeFloat(Val); }
	static FString ToString(double Val) { return FString::Printf(TEXT("%lf"), Val); }
	static FString ToString(const FString& Val) { return Val; }
	static FString ToString(const FName& Val) { return Val.ToString(); }
	static FString ToString(const FText& Val) { return Val.ToString(); }
	template <typename T>
    static FString ToString(T* Val) { return Val->ToString(); }	

	template <class PropType, typename ValueType>
	static typename SpudTypeInfo<ValueType>::StorageType WritePropertyData(
		PropType* Prop,
		uint32 PrefixID,
		const void* Data,
		bool bIsArrayElement,
		TSharedPtr<FSpudClassDef> ClassDef,
		TPrefixedPropertyOffsets& InPrefixToPropertyOffsets,
		FSpudClassMetadata& Meta,
		FArchive& Out)
	{
    	if (!bIsArrayElement)
    		RegisterProperty(Prop, PrefixID, ClassDef, InPrefixToPropertyOffsets, Meta, Out);
    	auto Val = static_cast<typename SpudTypeInfo<ValueType>::StorageType>(Prop->GetPropertyValue(Data)); // Cast in case we want to compress into smaller type
    	Out << Val;
    	return Val;
    }


	template <class PropType, typename ValueType>
	static bool TryWritePropertyData(FProperty* Prop, uint32 PrefixID, const void* Data, bool bIsArrayElement, int Depth,
	                          TSharedPtr<FSpudClassDef> ClassDef, TPrefixedPropertyOffsets& PrefixToPropertyOffsets, FSpudClassMetadata& Meta,
	                          FArchive& Out)
    {
    	if (auto IProp = CastField<PropType>(Prop))
    	{
    		auto Val = WritePropertyData<PropType, ValueType>(IProp, PrefixID, Data, bIsArrayElement, ClassDef, PrefixToPropertyOffsets, Meta, Out);
			UE_LOG(LogSpudProps, Verbose, TEXT("%s = %s"), *GetLogPrefix(Prop, Depth), *ToString(Val));
    		return true;
    	}
    	return false;
	    
    }

	static uint16 WriteEnumPropertyData(FEnumProperty* EProp,
	                                    uint32 PrefixID,
	                                    const void* Data,
	                                    bool bIsArrayElement,
	                                    TSharedPtr<FSpudClassDef> ClassDef,
	                                    TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                                    FSpudClassMetadata& Meta,
	                                    FArchive& Out);

	static bool TryWriteEnumPropertyData(FProperty* Property,
	                                     uint32 PrefixID,
	                                     const void* Data,
	                                     bool bIsArrayElement,
	                                     int Depth,
	                                     TSharedPtr<FSpudClassDef> ClassDef,
	                                     TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                                     FSpudClassMetadata& Meta,
	                                     FArchive& Out);
	static bool TryWriteSoftObjectPropertyData(FProperty* Property,
											   uint32 PrefixID,
											   const void* Data,
											   bool bIsArrayElement,
										       int Depth,
										       TSharedPtr<FSpudClassDef> ClassDef,
										       TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
										       const AActor* referencingActor,
										       const FWorldReferenceLookups& WorldReferenceLookups,
										       FSpudClassMetadata& Meta,
										       FArchive& Out);
	static FString WriteActorRefPropertyData(FObjectProperty* OProp,
	                                         AActor* Actor,
	                                         FPlatformTypes::uint32 PrefixID,
	                                         const void* Data,
	                                         bool bIsArrayElement,
	                                         TSharedPtr<FSpudClassDef> ClassDef,
											 TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
											 const AActor* referencingActor,
											 const FWorldReferenceLookups& WorldReferenceLookups,
	                                         FSpudClassMetadata& Meta,
	                                         FArchive& Out);
	static FString WriteNestedUObjectPropertyData(FObjectProperty* OProp,
	                                              UObject* UObj,
	                                              FPlatformTypes::uint32 PrefixID,
	                                              const void* Data,
	                                              bool bIsArrayElement,
	                                              TSharedPtr<FSpudClassDef> ClassDef,
	                                              TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                                              FSpudClassMetadata& Meta,
	                                              FArchive& Out);
	static FString WriteSubclassOfPropertyData(FClassProperty* CProp,
	                                           UClass* Class,
	                                           uint32 PrefixID,
	                                           const void* Data,
	                                           bool bIsArrayElement,
	                                           TSharedPtr<FSpudClassDef> ClassDef,
	                                           TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                                           FSpudClassMetadata& Meta,
	                                           FArchive& Out);
	static bool TryWriteUObjectPropertyData(FProperty* Property,
	                                        uint32 PrefixID,
	                                        const void* Data,
	                                        bool bIsArrayElement,
	                                        int Depth,
	                                        TSharedPtr<FSpudClassDef> ClassDef,
	                                        TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
											const AActor* referencingActor,
											const FWorldReferenceLookups& WorldReferenceLookups,
	                                        FSpudClassMetadata& Meta,
											FArchive& Out);
											
	static bool TryWriteMulticastDelegatePropertyData(FProperty* Property, 
													  uint32 PrefixID, 
													  const void* Data, 
													  bool bIsArrayElement,
	                                       			  int Depth, 
													  TSharedPtr<FSpudClassDef> ClassDef, 
													  TPrefixedPropertyOffsets& PrefixToPropertyOffsets,
	                                       			  const AActor* referencingActor,
	                                       			  const FWorldReferenceLookups& WorldReferenceLookups,
	                                       			  FSpudClassMetadata& Meta,
	                                       			  FArchive& Out);

	
	template<typename ValueType>
	static ValueType WriteStructPropertyData(FStructProperty* SProp, uint32 PrefixID, const void* Data, FArchive& Out)
    {
    	auto ValPtr = static_cast<const ValueType*>(Data);
    	// Need to explicitly copy because << can read as well and incoming data is const
    	ValueType Val = *ValPtr;
    	Out << Val;
    	return Val;
    }
	template <typename ValueType>
	static bool TryWriteBuiltinStructPropertyData(FStructProperty* Prop, uint32 PrefixID, const void* Data, bool bIsArrayElement,
	                                       int Depth, TSharedPtr<FSpudClassDef> ClassDef, TPrefixedPropertyOffsets& PrefixToPropertyOffsets, FSpudClassMetadata& Meta, FArchive& Out)
    {
    	// Check struct detail value matches
    	if (Prop->Struct == TBaseStructure<ValueType>::Get())
    	{
    		if (!bIsArrayElement)
    			RegisterProperty(Prop, PrefixID, ClassDef, PrefixToPropertyOffsets, Meta, Out);
    		ValueType Val = WriteStructPropertyData<ValueType>(Prop, PrefixID, Data, Out);
    		UE_LOG(LogSpudProps, Verbose, TEXT("%s = %s"), *GetLogPrefix(Prop, Depth), *ToString(&Val));
    		return true;
    	}
    	return false;
    }


	template<typename ValueType>
    static ValueType ReadStructPropertyData(FStructProperty* SProp, void* Data, FArchive& In)
	{
		auto ValPtr = static_cast<ValueType*>(Data);
		// In read mode, this should update pointer target (ugh I don't like UE dual-mode archvies)
		In << *ValPtr;
		return *ValPtr;
	}
	
	template <class PropType, typename ValueType>
    static typename SpudTypeInfo<ValueType>::StorageType ReadPropertyData(PropType* Prop, void* Data, FArchive& In)
	{
		// Read as per storage type
		typename SpudTypeInfo<ValueType>::StorageType Val;
		In << Val;
		// reverse the conversion we applied when writing to set property
		Prop->SetPropertyValue(Data, static_cast<ValueType>(Val));
		return Val;
	}


	template <typename ValueType>
    static bool TryReadBuiltinStructPropertyData(FStructProperty* Prop, void* Data, const FSpudPropertyDef& StoredProperty, int Depth, FArchive& In)
	{
		// Check runtime property and stored match 
		if (Prop->Struct == TBaseStructure<ValueType>::Get() &&
			StoredPropertyTypeMatchesRuntime(Prop, StoredProperty, true)) // we ignore array flag since we could be processing inner
		{
			ValueType Val = ReadStructPropertyData<ValueType>(Prop, Data, In);
    		UE_LOG(LogSpudProps, Verbose, TEXT("%s = %s"), *GetLogPrefix(Prop, Depth), *ToString(&Val));
			return true;
		}
		return false;
	}
	template <class PropType, typename ValueType>
    static bool TryReadPropertyData(FProperty* Prop, void* Data, const FSpudPropertyDef& StoredProperty, int Depth, FArchive& In)
	{
		auto IProp = CastField<PropType>(Prop);
		if (IProp && StoredPropertyTypeMatchesRuntime(Prop, StoredProperty, true)) // we ignore array flag since we could be processing inner
		{
			auto Val = ReadPropertyData<PropType, ValueType>(IProp, Data, In);
    		UE_LOG(LogSpudProps, Verbose, TEXT("%s = %s"), *GetLogPrefix(Prop, Depth), *ToString(Val));
			return true;
		}
		return false;   
	}

	static uint16 ReadEnumPropertyData(FEnumProperty* EProp, void* Data, FArchive& In);
	static bool TryReadEnumPropertyData(FProperty* Prop, void* Data, const FSpudPropertyDef& StoredProperty,
	                                    int Depth, FArchive& In);
	static bool TryReadSoftObjectPropertyData(FProperty* Prop, void* Data, const FSpudPropertyDef& StoredProperty,
											  const FWorldReferenceLookups& WorldReferenceLookups,
										      ULevel* Level, const FSpudClassMetadata& Meta, int Depth, FArchive& In);
	static bool TryReadMulticastDelegatePropertyData(FProperty* Prop, void* Data, const FSpudPropertyDef& StoredProperty,
													const FWorldReferenceLookups& WorldReferenceLookups,
													ULevel* Level, const FSpudClassMetadata& Meta, int Depth, FArchive& In);
	static FString ReadActorRefPropertyData(::FObjectProperty* OProp, void* Data, const FWorldReferenceLookups& WorldReferenceLookups, ULevel* Level, FArchive& In);
	static FString ReadNestedUObjectPropertyData(::FObjectProperty* OProp, void* Data, const FWorldReferenceLookups& WorldReferenceLookups,
		ULevel* Level, UObject* Outer, const FSpudClassMetadata& Meta, FArchive& In);
	static FString ReadSubclassOfPropertyData(::FObjectProperty* OProp, void* Data, const FWorldReferenceLookups& WorldReferenceLookups,
		ULevel* Level, const FSpudClassMetadata& Meta, FArchive& In);
	static bool TryReadUObjectPropertyData(::FProperty* Prop, void* Data, const ::FSpudPropertyDef& StoredProperty,
	                                        const FWorldReferenceLookups& WorldReferenceLookups,
	                                        ULevel* Level, UObject* Outer, const FSpudClassMetadata& Meta, int Depth, FArchive& In);

public:

	// Low-level functions, use with caution

	template <typename T>
    static void WriteRaw(const T& Value, FArchive& Out)
	{
		typename SpudTypeInfo<T>::StorageType OutVal = Value;
		Out << OutVal;		
	}

	template <typename T>
    static void ReadRaw(T& Value, FArchive& In)
	{
		// Allow for type conversion e.g. bool to uint8
		typename SpudTypeInfo<T>::StorageType SerialisedVal;
		In << SerialisedVal;
		Value = static_cast<T>(SerialisedVal);
	}

	template <typename T>
	void WriteProperty(const FString& Name, uint32 PrefixID, const T& Value, TSharedPtr<FSpudClassDef> ClassDef,
	                   TArray<uint32>& PropertyOffsets, FSpudClassMetadata& Meta, FArchive& Out)
	{
		RegisterProperty(Name, PrefixID, SpudTypeInfo<T>::EnumType, ClassDef, PropertyOffsets, Meta, Out);
		WriteRaw(Value, Out);
	}

	static AActor* GetReferencedActor(const FString& LevelRefString, const FString& ActorRefString,
									  const FWorldReferenceLookups& WorldReferenceLookups,
									  ULevel* Level, const FString& ReferencingPropertyName)
	{
		AActor* FoundActor = nullptr;
		// Now we need to find the actual object
		if(!ActorRefString.IsEmpty())
		{
			if (ActorRefString.StartsWith("{"))
			{
				// Runtime object, identified by GUID
				// We used the braces-format GUID for runtime objects so that it's easy to identify
				if (WorldReferenceLookups.RuntimeObjectMap)
				{
					FGuid Guid;
					if (FGuid::ParseExact(ActorRefString, EGuidFormats::DigitsWithHyphensInBraces, Guid))
					{
						auto ObjPtr = WorldReferenceLookups.RuntimeObjectMap->Find(Guid);
						if (ObjPtr)
						{
							FoundActor = CastChecked<AActor>(*ObjPtr);
						}
						else
						{
							UE_LOG(LogSpudProps, Error, TEXT("Could not locate runtime object for property %s, GUID was %s"), *ReferencingPropertyName, *ActorRefString);
						}			
					}
					else
					{
						UE_LOG(LogSpudProps, Error, TEXT("Error parsing GUID %s for property %s"), *ActorRefString, *ReferencingPropertyName);
					}
				}
				else
				{
					UE_LOG(LogSpudProps, Error, TEXT("Found property reference to runtime object %s->%s but no RuntimeObjects passed (global object?)"), *ReferencingPropertyName, *ActorRefString);
				}
			}
			else
			{
				// If LevelRefString is set, try to lookup a current loaded level to search there
				if(!LevelRefString.IsEmpty() && WorldReferenceLookups.WorldLevelsMap)
				{
					ULevel* const * foundLevel = WorldReferenceLookups.WorldLevelsMap->Find(LevelRefString);
					if(foundLevel)
					{
						Level = *foundLevel;
					}
					else
					{
						// Null the level pointer so the next stage will throw an error.
						Level = nullptr;

						// RETAIL FIX: Try to resolve name issues with levels named {name}_LevelInstance_4
						const int32 levelInstStart = LevelRefString.Find(TEXT("_LevelInstance_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
						if(levelInstStart != INDEX_NONE)
						{
							UE_LOG(LogSpudProps, Warning, TEXT("Fixed level reference string containing LevelInstance postfix in property '%s', before fixed named '%s'"), *ReferencingPropertyName, *LevelRefString);
							FString newLevelName = LevelRefString.Left(levelInstStart);

							const FString* patchedName = WorldReferenceLookups.PatchNamesMapping->Find(newLevelName);
							if(patchedName)
							{
								newLevelName = *patchedName;
							}
							
							foundLevel = WorldReferenceLookups.WorldLevelsMap->Find(newLevelName);
							if(foundLevel)
							{
								Level = *foundLevel;
							}
						}
						else
						{
							const FString* patchedName = WorldReferenceLookups.PatchNamesMapping->Find(LevelRefString);
							if(patchedName)
							{
								foundLevel = WorldReferenceLookups.WorldLevelsMap->Find(*patchedName);
								if(foundLevel)
								{
									Level = *foundLevel;
								}
							}
						}
					}
				}
				
				// Level object, identified by name. Level is the package
				if (Level)
				{
					auto Obj = StaticFindObject(AActor::StaticClass(), Level, *ActorRefString);
					if (Obj)
					{
						FoundActor = CastChecked<AActor>(Obj);
					}
					else
					{
						UE_LOG(LogSpudProps, Error, TEXT("Could not locate level '%s' object for property '%s', actor name was '%s'"),
						       LevelRefString.IsEmpty() ? TEXT("LOCAL") : *LevelRefString, *ReferencingPropertyName, *ActorRefString);
					}
				}
				else
				{
					UE_LOG(LogSpudProps, Error, TEXT("Level '%s' object for property '%s' cannot be resolved, null Level. Is the level loaded? Actor name was '%s'"),
						   LevelRefString.IsEmpty() ? TEXT("LOCAL") : *LevelRefString, *ReferencingPropertyName, *ActorRefString);
				}
			}
		}

		return FoundActor;
	}

	// Attempts to resolve an actor by reference and assign it to the passed in property
	template <typename T>
	static bool AssignReferencedActorToProperty(const FString& LevelRefString, const FString& ActorRefString,
												const FWorldReferenceLookups& WorldReferenceLookups,
												ULevel* Level, const T* ObjProp, void* Data)
	{
		// Now we need to find the actual object
		AActor* foundActor = nullptr;
		if (!ActorRefString.IsEmpty())
		{
			foundActor = GetReferencedActor(LevelRefString, ActorRefString, WorldReferenceLookups, Level, ObjProp->GetName());
		}

		ObjProp->SetObjectPropertyValue(Data, foundActor);
		return true;
	}

	/// Return whether this object is persistent. Null safe
	static bool IsPersistentObject(UObject* Obj);
	/// Return whether an actor is a runtime created one, or whether it was part of a loaded level. Null safe
	static bool IsRuntimeActor(AActor* Actor);
	/// Get an actors referencing string that can be used at load time to re-reference the actor.
	/// Returns true if the reference if valid to use, otherwise false if the reference could not be created.
	/// Will return true if the Actor was null, and RefStringOut will be an empty string.
	static bool GetActorReferenceString(AActor* Actor, const AActor* referencingActor,
									    const FWorldReferenceLookups& WorldReferenceLookups,
									    FString& LevelReferenceString, FString& ActorReferenceString);
	/// Get the SpudGuid property value of an object, if it has one (blank otherwise)
	static FGuid GetGuidProperty(const UObject* Obj);
	/// Get the SpudGuid property value of an object, from a previously found property (blank if null)
	static FGuid GetGuidProperty(const UObject* Obj, const FStructProperty* Prop);
	/// Set the SpudGuid property value of an object, if it has one. Returns whether it was found & set
	static bool SetGuidProperty(UObject* Obj, const FGuid& Guid);
	/// Set the SpudGuid property value of an object, using previously found property. Returns whether it was found & set
	static bool SetGuidProperty(UObject* Obj, const FStructProperty* Prop, const FGuid& Guid);
	/// Get the SpudGuid property on a object, if it exists (null otherwise)
	static FStructProperty* FindGuidProperty(const UObject* Obj);
	/// Get the unique name of an actor within a level
	static FString GetLevelActorName(const AActor* Actor);
	/// Get the identifier to use for a global object 
	static FString GetGlobalObjectID(const UObject* Obj);
	/// Get the class name of an object 
	static FString GetClassName(const UObject* Obj);

	static FString GetLogPrefix(int Depth);
	static FString GetLogPrefix(const FProperty* Property, int Depth);

};

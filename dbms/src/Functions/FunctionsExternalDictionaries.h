#pragma once

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeUUID.h>

#include <Common/typeid_cast.h>

#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>

#include <Interpreters/Context.h>
#include <Interpreters/ExternalDictionaries.h>

#include <Functions/IFunction.h>
#include <Functions/FunctionHelpers.h>

#include <Dictionaries/FlatDictionary.h>
#include <Dictionaries/HashedDictionary.h>
#include <Dictionaries/CacheDictionary.h>
#include <Dictionaries/ComplexKeyHashedDictionary.h>
#include <Dictionaries/ComplexKeyCacheDictionary.h>
#include <Dictionaries/RangeHashedDictionary.h>
#include <Dictionaries/TrieDictionary.h>

#include <ext/range.h>

#include <type_traits>

namespace DB
{

namespace ErrorCodes
{
    extern const int DICTIONARIES_WAS_NOT_LOADED;
    extern const int UNSUPPORTED_METHOD;
    extern const int UNKNOWN_TYPE;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int TYPE_MISMATCH;
}

/** Functions that use plug-ins (external) dictionaries.
  *
  * Get the value of the attribute of the specified type.
  *     dictGetType(dictionary, attribute, id),
  *         Type - placeholder for the type name, any numeric and string types are currently supported.
  *        The type must match the actual attribute type with which it was declared in the dictionary structure.
  *
  * Get an array of identifiers, consisting of the source and parents chain.
  *  dictGetHierarchy(dictionary, id).
  *
  * Is the first identifier the child of the second.
  *  dictIsIn(dictionary, child_id, parent_id).
  */


class FunctionDictHas final : public IFunction
{
public:
    static constexpr auto name = "dictHas";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictHas>(context.getExternalDictionaries());
    }

    FunctionDictHas(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 2; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[1]).isUInt64() &&
            !isTuple(arguments[1]))
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataTypeUInt8>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        /** Do not require existence of the dictionary if the function is called for empty block.
          * This is needed to allow successful query analysis on a server,
          *  that is the initiator of a distributed query,
          *  in the case when the function will be invoked for real data only at the remote servers.
          * This feature is controversial and implemented specially
          *  for backward compatibility with the case in Yandex Banner System.
          */
        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatchSimple<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchSimple<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchSimple<CacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyHashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyCacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<TrieDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatchSimple(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const auto id_col_untyped = block.getByPosition(arguments[1]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
        {
            const auto & ids = id_col->getData();

            auto out = ColumnUInt8::create(ext::size(ids));
            dict->has(ids, out->getData());
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Second argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    bool executeDispatchComplex(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const ColumnWithTypeAndName & key_col_with_type = block.getByPosition(arguments[1]);
        const ColumnPtr & key_col = key_col_with_type.column;

        if (checkColumn<ColumnTuple>(key_col.get()))
        {
            const auto & key_columns = static_cast<const ColumnTuple &>(*key_col).getColumns();
            const auto & key_types = static_cast<const DataTypeTuple &>(*key_col_with_type.type).getElements();

            auto out = ColumnUInt8::create(key_col_with_type.column->size());
            dict->has(key_columns, key_types, out->getData());
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Second argument of function " + getName() + " must be " + dict->getKeyDescription(), ErrorCodes::TYPE_MISMATCH};

        return true;
    }

    const ExternalDictionaries & dictionaries;
};


static bool isDictGetFunctionInjective(const ExternalDictionaries & dictionaries, const Block & sample_block)
{
    if (sample_block.columns() != 3 && sample_block.columns() != 4)
        throw Exception{"Function dictGet... takes 3 or 4 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

    const auto dict_name_col = checkAndGetColumnConst<ColumnString>(sample_block.getByPosition(0).column.get());
    if (!dict_name_col)
        throw Exception{"First argument of function dictGet... must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

    const auto attr_name_col = checkAndGetColumnConst<ColumnString>(sample_block.getByPosition(1).column.get());
    if (!attr_name_col)
        throw Exception{"Second argument of function dictGet... must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

    return dictionaries.getDictionary(dict_name_col->getValue<String>())->isInjective(attr_name_col->getValue<String>());
}


/** For ColumnVector. Either returns a reference to internal data,
  *  or convert it to T type, stores the result in backup_storage and returns a reference to it.
  */
template <typename T>
static const PaddedPODArray<T> & getColumnDataAsPaddedPODArray(const IColumn & column, PaddedPODArray<T> & backup_storage);


class FunctionDictGetString final : public IFunction
{
public:
    static constexpr auto name = "dictGetString";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetString>(context.getExternalDictionaries());
    }

    FunctionDictGetString(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    bool isInjective(const Block & sample_block) override
    {
        return isDictGetFunctionInjective(dictionaries, sample_block);
    }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 3 && arguments.size() != 4)
            throw Exception{"Number of arguments for function " + getName() + " doesn't match: passed "
                + toString(arguments.size()) + ", should be 3 or 4.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        if (!isString(arguments[0]))
        {
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        if (!isString(arguments[1]))
        {
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        if (!WhichDataType(arguments[2]).isUInt64() &&
            !isTuple(arguments[2]))
        {
            throw Exception{"Illegal type " + arguments[2]->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        /// This is for the case of range dictionaries.
        if (arguments.size() == 4 && !arguments[3]->isValueRepresentedByInteger())
        {
            throw Exception{"Illegal type " + arguments[3]->getName() +
                            " of fourth argument of function " + getName() +
                            " must be convertible to Int64.", ErrorCodes::ILLEGAL_COLUMN};
        }

        return std::make_shared<DataTypeString>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyHashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyCacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<TrieDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchRange<RangeHashedDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 3)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 3 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto id_col_untyped = block.getByPosition(arguments[2]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
        {
            auto out = ColumnString::create();
            dict->getString(attr_name, id_col->getData(), out.get());
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Third argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    bool executeDispatchComplex(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 3)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 3 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const ColumnWithTypeAndName & key_col_with_type = block.getByPosition(arguments[2]);
        ColumnPtr key_col = key_col_with_type.column;

        /// Functions in external dictionaries only support full-value (not constant) columns with keys.
        if (ColumnPtr key_col_materialized = key_col_with_type.column->convertToFullColumnIfConst())
            key_col = key_col_materialized;

        if (checkColumn<ColumnTuple>(key_col.get()))
        {
            const auto & key_columns = static_cast<const ColumnTuple &>(*key_col).getColumns();
            const auto & key_types = static_cast<const DataTypeTuple &>(*key_col_with_type.type).getElements();

            auto out = ColumnString::create();
            dict->getString(attr_name, key_columns, key_types, out.get());
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Third argument of function " + getName() + " must be " + dict->getKeyDescription(), ErrorCodes::TYPE_MISMATCH};

        return true;
    }

    template <typename DictionaryType>
    bool executeDispatchRange(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 4)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 4 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto & id_col_untyped = block.getByPosition(arguments[2]).column;
        const auto & range_col_untyped = block.getByPosition(arguments[3]).column;

        PaddedPODArray<UInt64> id_col_values_storage;
        PaddedPODArray<Int64> range_col_values_storage;
        const auto & id_col_values = getColumnDataAsPaddedPODArray(*id_col_untyped, id_col_values_storage);
        const auto & range_col_values = getColumnDataAsPaddedPODArray(*range_col_untyped, range_col_values_storage);

        auto out = ColumnString::create();
        dict->getString(attr_name, id_col_values, range_col_values, out.get());
        block.getByPosition(result).column = std::move(out);

        return true;
    }

    const ExternalDictionaries & dictionaries;
};


class FunctionDictGetStringOrDefault final : public IFunction
{
public:
    static constexpr auto name = "dictGetStringOrDefault";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetStringOrDefault>(context.getExternalDictionaries());
    }

    FunctionDictGetStringOrDefault(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 4; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName() +
                ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!isString(arguments[1]))
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName() +
                ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2]).isUInt64() &&
            !isTuple(arguments[2]))
        {
            throw Exception{"Illegal type " + arguments[2]->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        if (!isString(arguments[3]))
            throw Exception{"Illegal type " + arguments[3]->getName() + " of fourth argument of function " + getName() +
                ", must be String.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataTypeString>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyHashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyCacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<TrieDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto id_col_untyped = block.getByPosition(arguments[2]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
            executeDispatch(block, arguments, result, dict, attr_name, id_col);
        else if (const auto id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(id_col_untyped))
            executeDispatch(block, arguments, result, dict, attr_name, id_col);
        else
            throw Exception{"Third argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    void executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const DictionaryType * dictionary,
        const std::string & attr_name, const ColumnUInt64 * id_col)
    {
        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();

        if (const auto default_col = checkAndGetColumn<ColumnString>(default_col_untyped))
        {
            /// vector ids, vector defaults
            auto out = ColumnString::create();
            const auto & ids = id_col->getData();
            dictionary->getString(attr_name, ids, default_col, out.get());
            block.getByPosition(result).column = std::move(out);
        }
        else if (const auto default_col = checkAndGetColumnConstStringOrFixedString(default_col_untyped))
        {
            /// vector ids, const defaults
            auto out = ColumnString::create();
            const auto & ids = id_col->getData();
            String def = default_col->getValue<String>();
            dictionary->getString(attr_name, ids, def, out.get());
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be String", ErrorCodes::ILLEGAL_COLUMN};
    }

    template <typename DictionaryType>
    void executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const DictionaryType * dictionary,
        const std::string & attr_name, const ColumnConst * id_col)
    {
        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();

        if (const auto default_col = checkAndGetColumn<ColumnString>(default_col_untyped))
        {
            /// const ids, vector defaults
            const PaddedPODArray<UInt64> ids(1, id_col->getValue<UInt64>());
            PaddedPODArray<UInt8> flags(1);
            dictionary->has(ids, flags);
            if (flags.front())
            {
                auto out = ColumnString::create();
                dictionary->getString(attr_name, ids, String(), out.get());
                block.getByPosition(result).column = DataTypeString().createColumnConst(id_col->size(), out->getDataAt(0).toString());
            }
            else
                block.getByPosition(result).column = block.getByPosition(arguments[3]).column; // reuse the default column
        }
        else if (const auto default_col = checkAndGetColumnConstStringOrFixedString(default_col_untyped))
        {
            /// const ids, const defaults
            const PaddedPODArray<UInt64> ids(1, id_col->getValue<UInt64>());
            auto out = ColumnString::create();
            String def = default_col->getValue<String>();
            dictionary->getString(attr_name, ids, def, out.get());
            block.getByPosition(result).column = DataTypeString().createColumnConst(id_col->size(), out->getDataAt(0).toString());
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be String", ErrorCodes::ILLEGAL_COLUMN};
    }

    template <typename DictionaryType>
    bool executeDispatchComplex(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const ColumnWithTypeAndName & key_col_with_type = block.getByPosition(arguments[2]);
        ColumnPtr key_col = key_col_with_type.column;

        /// Functions in external dictionaries only support full-value (not constant) columns with keys.
        if (ColumnPtr key_col_materialized = key_col_with_type.column->convertToFullColumnIfConst())
            key_col = key_col_materialized;

        const auto & key_columns = typeid_cast<const ColumnTuple &>(*key_col).getColumns();
        const auto & key_types = static_cast<const DataTypeTuple &>(*key_col_with_type.type).getElements();

        auto out = ColumnString::create();

        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();
        if (const auto default_col = checkAndGetColumn<ColumnString>(default_col_untyped))
        {
            dict->getString(attr_name, key_columns, key_types, default_col, out.get());
        }
        else if (const auto default_col = checkAndGetColumnConstStringOrFixedString(default_col_untyped))
        {
            String def = default_col->getValue<String>();
            dict->getString(attr_name, key_columns, key_types, def, out.get());
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be String", ErrorCodes::ILLEGAL_COLUMN};

        block.getByPosition(result).column = std::move(out);
        return true;
    }

    const ExternalDictionaries & dictionaries;
};


template <typename DataType> struct DictGetTraits;
#define DECLARE_DICT_GET_TRAITS(TYPE, DATA_TYPE) \
template <> struct DictGetTraits<DATA_TYPE>\
{\
    template <typename DictionaryType>\
    static void get(\
        const DictionaryType * dict, const std::string & name, const PaddedPODArray<UInt64> & ids,\
        PaddedPODArray<TYPE> & out)\
    {\
        dict->get##TYPE(name, ids, out);\
    }\
    template <typename DictionaryType>\
    static void get(\
        const DictionaryType * dict, const std::string & name, const Columns & key_columns,\
        const DataTypes & key_types, PaddedPODArray<TYPE> & out)\
    {\
        dict->get##TYPE(name, key_columns, key_types, out);\
    }\
    template <typename DictionaryType>\
    static void get(\
        const DictionaryType * dict, const std::string & name, const PaddedPODArray<UInt64> & ids,\
        const PaddedPODArray<Int64> & dates, PaddedPODArray<TYPE> & out)\
    {\
        dict->get##TYPE(name, ids, dates, out);\
    }\
    template <typename DictionaryType, typename DefaultsType>\
    static void getOrDefault(\
        const DictionaryType * dict, const std::string & name, const PaddedPODArray<UInt64> & ids,\
        const DefaultsType & def, PaddedPODArray<TYPE> & out)\
    {\
        dict->get##TYPE(name, ids, def, out);\
    }\
    template <typename DictionaryType, typename DefaultsType>\
    static void getOrDefault(\
        const DictionaryType * dict, const std::string & name, const Columns & key_columns,\
        const DataTypes & key_types, const DefaultsType & def, PaddedPODArray<TYPE> & out)\
    {\
        dict->get##TYPE(name, key_columns, key_types, def, out);\
    }\
};
DECLARE_DICT_GET_TRAITS(UInt8, DataTypeUInt8)
DECLARE_DICT_GET_TRAITS(UInt16, DataTypeUInt16)
DECLARE_DICT_GET_TRAITS(UInt32, DataTypeUInt32)
DECLARE_DICT_GET_TRAITS(UInt64, DataTypeUInt64)
DECLARE_DICT_GET_TRAITS(Int8, DataTypeInt8)
DECLARE_DICT_GET_TRAITS(Int16, DataTypeInt16)
DECLARE_DICT_GET_TRAITS(Int32, DataTypeInt32)
DECLARE_DICT_GET_TRAITS(Int64, DataTypeInt64)
DECLARE_DICT_GET_TRAITS(Float32, DataTypeFloat32)
DECLARE_DICT_GET_TRAITS(Float64, DataTypeFloat64)
DECLARE_DICT_GET_TRAITS(UInt16, DataTypeDate)
DECLARE_DICT_GET_TRAITS(UInt32, DataTypeDateTime)
DECLARE_DICT_GET_TRAITS(UInt128, DataTypeUUID)
#undef DECLARE_DICT_GET_TRAITS


template <typename DataType, typename Name>
class FunctionDictGet final : public IFunction
{
    using Type = typename DataType::FieldType;

public:
    static constexpr auto name = Name::name;

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGet>(context.getExternalDictionaries());
    }

    FunctionDictGet(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    bool isInjective(const Block & sample_block) override
    {
        return isDictGetFunctionInjective(dictionaries, sample_block);
    }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() != 3 && arguments.size() != 4)
            throw Exception{"Function " + getName() + " takes 3 or 4 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!isString(arguments[1]))
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2]).isUInt64() &&
            !isTuple(arguments[2]))
            throw Exception{"Illegal type " + arguments[2]->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (arguments.size() == 4)
        {
            const auto range_argument = arguments[3].get();
            if (!(range_argument->isValueRepresentedByInteger() &&
                   range_argument->getSizeOfValueInMemory() <= sizeof(Int64)))
                throw Exception{"Illegal type " + range_argument->getName() + " of fourth argument of function " + getName()
                    + ", must be convertible to " + TypeName<Int64>::get() + ".",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        return std::make_shared<DataType>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyHashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyCacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<TrieDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchRange<RangeHashedDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(Block & block, const ColumnNumbers & arguments, const size_t result,
        const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 3)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 3 arguments.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto id_col_untyped = block.getByPosition(arguments[2]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
        {
            auto out = ColumnVector<Type>::create(id_col->size());
            const auto & ids = id_col->getData();
            auto & data = out->getData();
            DictGetTraits<DataType>::get(dict, attr_name, ids, data);
            block.getByPosition(result).column = std::move(out);
        }
        else if (const auto id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(id_col_untyped))
        {
            const PaddedPODArray<UInt64> ids(1, id_col->getValue<UInt64>());
            PaddedPODArray<Type> data(1);
            DictGetTraits<DataType>::get(dict, attr_name, ids, data);
            block.getByPosition(result).column = DataTypeNumber<Type>().createColumnConst(id_col->size(), toField(data.front()));
        }
        else
            throw Exception{"Third argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    bool executeDispatchComplex(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 3)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 3 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const ColumnWithTypeAndName & key_col_with_type = block.getByPosition(arguments[2]);
        ColumnPtr key_col = key_col_with_type.column;

        /// Functions in external dictionaries only support full-value (not constant) columns with keys.
        if (ColumnPtr key_col_materialized = key_col_with_type.column->convertToFullColumnIfConst())
            key_col = key_col_materialized;

        if (checkColumn<ColumnTuple>(key_col.get()))
        {
            const auto & key_columns = static_cast<const ColumnTuple &>(*key_col).getColumns();
            const auto & key_types = static_cast<const DataTypeTuple &>(*key_col_with_type.type).getElements();

            auto out = ColumnVector<Type>::create(key_columns.front()->size());
            auto & data = out->getData();
            DictGetTraits<DataType>::get(dict, attr_name, key_columns, key_types, data);
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Third argument of function " + getName() + " must be " + dict->getKeyDescription(), ErrorCodes::TYPE_MISMATCH};

        return true;
    }

    template <typename DictionaryType>
    bool executeDispatchRange(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (arguments.size() != 4)
            throw Exception{"Function " + getName() + " for dictionary of type " + dict->getTypeName() +
                " requires exactly 4 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto & id_col_untyped = block.getByPosition(arguments[2]).column;
        const auto & range_col_untyped = block.getByPosition(arguments[3]).column;

        PaddedPODArray<UInt64> id_col_values_storage;
        PaddedPODArray<Int64> range_col_values_storage;
        const auto & id_col_values = getColumnDataAsPaddedPODArray(*id_col_untyped, id_col_values_storage);
        const auto & range_col_values = getColumnDataAsPaddedPODArray(*range_col_untyped, range_col_values_storage);

        auto out = ColumnVector<Type>::create(id_col_untyped->size());
        auto & data = out->getData();
        DictGetTraits<DataType>::get(dict, attr_name, id_col_values, range_col_values, data);
        block.getByPosition(result).column = std::move(out);

        return true;
    }

    const ExternalDictionaries & dictionaries;
};

struct NameDictGetUInt8 { static constexpr auto name = "dictGetUInt8"; };
struct NameDictGetUInt16 { static constexpr auto name = "dictGetUInt16"; };
struct NameDictGetUInt32 { static constexpr auto name = "dictGetUInt32"; };
struct NameDictGetUInt64 { static constexpr auto name = "dictGetUInt64"; };
struct NameDictGetInt8 { static constexpr auto name = "dictGetInt8"; };
struct NameDictGetInt16 { static constexpr auto name = "dictGetInt16"; };
struct NameDictGetInt32 { static constexpr auto name = "dictGetInt32"; };
struct NameDictGetInt64 { static constexpr auto name = "dictGetInt64"; };
struct NameDictGetFloat32 { static constexpr auto name = "dictGetFloat32"; };
struct NameDictGetFloat64 { static constexpr auto name = "dictGetFloat64"; };
struct NameDictGetDate { static constexpr auto name = "dictGetDate"; };
struct NameDictGetDateTime { static constexpr auto name = "dictGetDateTime"; };
struct NameDictGetUUID { static constexpr auto name = "dictGetUUID"; };

using FunctionDictGetUInt8 = FunctionDictGet<DataTypeUInt8, NameDictGetUInt8>;
using FunctionDictGetUInt16 = FunctionDictGet<DataTypeUInt16, NameDictGetUInt16>;
using FunctionDictGetUInt32 = FunctionDictGet<DataTypeUInt32, NameDictGetUInt32>;
using FunctionDictGetUInt64 = FunctionDictGet<DataTypeUInt64, NameDictGetUInt64>;
using FunctionDictGetInt8 = FunctionDictGet<DataTypeInt8, NameDictGetInt8>;
using FunctionDictGetInt16 = FunctionDictGet<DataTypeInt16, NameDictGetInt16>;
using FunctionDictGetInt32 = FunctionDictGet<DataTypeInt32, NameDictGetInt32>;
using FunctionDictGetInt64 = FunctionDictGet<DataTypeInt64, NameDictGetInt64>;
using FunctionDictGetFloat32 = FunctionDictGet<DataTypeFloat32, NameDictGetFloat32>;
using FunctionDictGetFloat64 = FunctionDictGet<DataTypeFloat64, NameDictGetFloat64>;
using FunctionDictGetDate = FunctionDictGet<DataTypeDate, NameDictGetDate>;
using FunctionDictGetDateTime = FunctionDictGet<DataTypeDateTime, NameDictGetDateTime>;
using FunctionDictGetUUID = FunctionDictGet<DataTypeUUID, NameDictGetUUID>;


template <typename DataType, typename Name>
class FunctionDictGetOrDefault final : public IFunction
{
    using Type = typename DataType::FieldType;

public:
    static constexpr auto name = Name::name;

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetOrDefault>(context.getExternalDictionaries());
    }

    FunctionDictGetOrDefault(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 4; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!isString(arguments[1]))
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2]).isUInt64() &&
            !isTuple(arguments[2]))
            throw Exception{"Illegal type " + arguments[2]->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!checkAndGetDataType<DataType>(arguments[3].get()))
            throw Exception{"Illegal type " + arguments[3]->getName() + " of fourth argument of function " + getName()
                + ", must be " + String(DataType{}.getFamilyName()) + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataType>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyHashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<ComplexKeyCacheDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatchComplex<TrieDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(Block & block, const ColumnNumbers & arguments, const size_t result,
        const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const auto id_col_untyped = block.getByPosition(arguments[2]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
            executeDispatch(block, arguments, result, dict, attr_name, id_col);
        else if (const auto id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(id_col_untyped))
            executeDispatch(block, arguments, result, dict, attr_name, id_col);
        else
            throw Exception{"Third argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    void executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const DictionaryType * dictionary,
        const std::string & attr_name, const ColumnUInt64 * id_col)
    {
        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();

        if (const auto default_col = checkAndGetColumn<ColumnVector<Type>>(default_col_untyped))
        {
            /// vector ids, vector defaults
            auto out = ColumnVector<Type>::create(id_col->size());
            const auto & ids = id_col->getData();
            auto & data = out->getData();
            const auto & defs = default_col->getData();
            DictGetTraits<DataType>::getOrDefault(dictionary, attr_name, ids, defs, data);
            block.getByPosition(result).column = std::move(out);
        }
        else if (const auto default_col = checkAndGetColumnConst<ColumnVector<Type>>(default_col_untyped))
        {
            /// vector ids, const defaults
            auto out = ColumnVector<Type>::create(id_col->size());
            const auto & ids = id_col->getData();
            auto & data = out->getData();
            const auto def = default_col->template getValue<Type>();
            DictGetTraits<DataType>::getOrDefault(dictionary, attr_name, ids, def, data);
            block.getByPosition(result).column = std::move(out);
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be " + String(DataType{}.getFamilyName()), ErrorCodes::ILLEGAL_COLUMN};
    }

    template <typename DictionaryType>
    void executeDispatch(
        Block & block, const ColumnNumbers & arguments, const size_t result, const DictionaryType * dictionary,
        const std::string & attr_name, const ColumnConst * id_col)
    {
        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();

        if (const auto default_col = checkAndGetColumn<ColumnVector<Type>>(default_col_untyped))
        {
            /// const ids, vector defaults
            const PaddedPODArray<UInt64> ids(1, id_col->getValue<UInt64>());
            PaddedPODArray<UInt8> flags(1);
            dictionary->has(ids, flags);
            if (flags.front())
            {
                PaddedPODArray<Type> data(1);
                DictGetTraits<DataType>::getOrDefault(dictionary, attr_name, ids, Type(), data);
                block.getByPosition(result).column = DataTypeNumber<Type>().createColumnConst(id_col->size(), toField(data.front()));
            }
            else
                block.getByPosition(result).column = block.getByPosition(arguments[3]).column; // reuse the default column
        }
        else if (const auto default_col = checkAndGetColumnConst<ColumnVector<Type>>(default_col_untyped))
        {
            /// const ids, const defaults
            const PaddedPODArray<UInt64> ids(1, id_col->getValue<UInt64>());
            PaddedPODArray<Type> data(1);
            const auto & def = default_col->template getValue<Type>();
            DictGetTraits<DataType>::getOrDefault(dictionary, attr_name, ids, def, data);
            block.getByPosition(result).column = DataTypeNumber<Type>().createColumnConst(id_col->size(), toField(data.front()));
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be " + String(DataType{}.getFamilyName()), ErrorCodes::ILLEGAL_COLUMN};
    }

    template <typename DictionaryType>
    bool executeDispatchComplex(
        Block & block, const ColumnNumbers & arguments, const size_t result, const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        const auto attr_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[1]).column.get());
        if (!attr_name_col)
            throw Exception{"Second argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        String attr_name = attr_name_col->getValue<String>();

        const ColumnWithTypeAndName & key_col_with_type = block.getByPosition(arguments[2]);
        ColumnPtr key_col = key_col_with_type.column;

        /// Functions in external dictionaries only support full-value (not constant) columns with keys.
        if (ColumnPtr key_col_materialized = key_col_with_type.column->convertToFullColumnIfConst())
            key_col = key_col_materialized;

        const auto & key_columns = typeid_cast<const ColumnTuple &>(*key_col).getColumns();
        const auto & key_types = static_cast<const DataTypeTuple &>(*key_col_with_type.type).getElements();

        /// @todo detect when all key columns are constant
        const auto rows = key_col->size();
        auto out = ColumnVector<Type>::create(rows);
        auto & data = out->getData();

        const auto default_col_untyped = block.getByPosition(arguments[3]).column.get();
        if (const auto default_col = checkAndGetColumn<ColumnVector<Type>>(default_col_untyped))
        {
            /// const defaults
            const auto & defs = default_col->getData();

            DictGetTraits<DataType>::getOrDefault(dict, attr_name, key_columns, key_types, defs, data);
        }
        else if (const auto default_col = checkAndGetColumnConst<ColumnVector<Type>>(default_col_untyped))
        {
            const auto def = default_col->template getValue<Type>();

            DictGetTraits<DataType>::getOrDefault(dict, attr_name, key_columns, key_types, def, data);
        }
        else
            throw Exception{"Fourth argument of function " + getName() + " must be " + String(DataType{}.getFamilyName()), ErrorCodes::ILLEGAL_COLUMN};

        block.getByPosition(result).column = std::move(out);
        return true;
    }

    const ExternalDictionaries & dictionaries;
};

struct NameDictGetUInt8OrDefault { static constexpr auto name = "dictGetUInt8OrDefault"; };
struct NameDictGetUInt16OrDefault { static constexpr auto name = "dictGetUInt16OrDefault"; };
struct NameDictGetUInt32OrDefault { static constexpr auto name = "dictGetUInt32OrDefault"; };
struct NameDictGetUInt64OrDefault { static constexpr auto name = "dictGetUInt64OrDefault"; };
struct NameDictGetInt8OrDefault { static constexpr auto name = "dictGetInt8OrDefault"; };
struct NameDictGetInt16OrDefault { static constexpr auto name = "dictGetInt16OrDefault"; };
struct NameDictGetInt32OrDefault { static constexpr auto name = "dictGetInt32OrDefault"; };
struct NameDictGetInt64OrDefault { static constexpr auto name = "dictGetInt64OrDefault"; };
struct NameDictGetFloat32OrDefault { static constexpr auto name = "dictGetFloat32OrDefault"; };
struct NameDictGetFloat64OrDefault { static constexpr auto name = "dictGetFloat64OrDefault"; };
struct NameDictGetDateOrDefault { static constexpr auto name = "dictGetDateOrDefault"; };
struct NameDictGetDateTimeOrDefault { static constexpr auto name = "dictGetDateTimeOrDefault"; };
struct NameDictGetUUIDOrDefault { static constexpr auto name = "dictGetUUIDOrDefault"; };

using FunctionDictGetUInt8OrDefault = FunctionDictGetOrDefault<DataTypeUInt8, NameDictGetUInt8OrDefault>;
using FunctionDictGetUInt16OrDefault = FunctionDictGetOrDefault<DataTypeUInt16, NameDictGetUInt16OrDefault>;
using FunctionDictGetUInt32OrDefault = FunctionDictGetOrDefault<DataTypeUInt32, NameDictGetUInt32OrDefault>;
using FunctionDictGetUInt64OrDefault = FunctionDictGetOrDefault<DataTypeUInt64, NameDictGetUInt64OrDefault>;
using FunctionDictGetInt8OrDefault = FunctionDictGetOrDefault<DataTypeInt8, NameDictGetInt8OrDefault>;
using FunctionDictGetInt16OrDefault = FunctionDictGetOrDefault<DataTypeInt16, NameDictGetInt16OrDefault>;
using FunctionDictGetInt32OrDefault = FunctionDictGetOrDefault<DataTypeInt32, NameDictGetInt32OrDefault>;
using FunctionDictGetInt64OrDefault = FunctionDictGetOrDefault<DataTypeInt64, NameDictGetInt64OrDefault>;
using FunctionDictGetFloat32OrDefault = FunctionDictGetOrDefault<DataTypeFloat32, NameDictGetFloat32OrDefault>;
using FunctionDictGetFloat64OrDefault = FunctionDictGetOrDefault<DataTypeFloat64, NameDictGetFloat64OrDefault>;
using FunctionDictGetDateOrDefault = FunctionDictGetOrDefault<DataTypeDate, NameDictGetDateOrDefault>;
using FunctionDictGetDateTimeOrDefault = FunctionDictGetOrDefault<DataTypeDateTime, NameDictGetDateTimeOrDefault>;
using FunctionDictGetUUIDOrDefault = FunctionDictGetOrDefault<DataTypeUUID, NameDictGetUUIDOrDefault>;

#define FOR_DICT_TYPES(M) \
    M(UInt8) \
    M(UInt16) \
    M(UInt32) \
    M(UInt64) \
    M(Int8) \
    M(Int16) \
    M(Int32) \
    M(Int64) \
    M(Float32) \
    M(Float64) \
    M(Date) \
    M(DateTime) \
    M(UUID)

/// This variant of function derives the result type automatically.
class FunctionDictGetNoType final : public IFunction
{
public:
    static constexpr auto name = "dictGet";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetNoType>(context.getExternalDictionaries(), context);
    }

    FunctionDictGetNoType(const ExternalDictionaries & dictionaries, const Context & context) : dictionaries(dictionaries), context(context) {}

    String getName() const override { return name; }

private:
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    bool isInjective(const Block & sample_block) override
    {
        return isDictGetFunctionInjective(dictionaries, sample_block);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 3 && arguments.size() != 4)
            throw Exception{"Function " + getName() + " takes 3 or 4 arguments", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH};

        String dict_name;
        if (auto name_col = checkAndGetColumnConst<ColumnString>(arguments[0].column.get()))
        {
            dict_name = name_col->getValue<String>();
        }
        else
            throw Exception{"Illegal type " + arguments[0].type->getName() + " of first argument of function " + getName()
                + ", expected a const string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        String attr_name;
        if (auto name_col = checkAndGetColumnConst<ColumnString>(arguments[1].column.get()))
        {
            attr_name = name_col->getValue<String>();
        }
        else
            throw Exception{"Illegal type " + arguments[1].type->getName() + " of second argument of function " + getName()
                + ", expected a const string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2].type).isUInt64() &&
            !isTuple(arguments[2].type))
            throw Exception{"Illegal type " + arguments[2].type->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (arguments.size() == 4)
        {
            const auto range_argument = arguments[3].type.get();
            if (!(range_argument->isValueRepresentedByInteger() &&
                   range_argument->getSizeOfValueInMemory() <= sizeof(Int64)))
                throw Exception{"Illegal type " + range_argument->getName() + " of fourth argument of function " + getName()
                    + ", must be convertible to " + TypeName<Int64>::get() + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        }

        auto dict = dictionaries.getDictionary(dict_name);
        const DictionaryStructure & structure = dict->getStructure();

        for (const auto idx : ext::range(0, structure.attributes.size()))
        {
            const DictionaryAttribute & attribute = structure.attributes[idx];
            if (attribute.name == attr_name)
            {
                WhichDataType dt = attribute.type;
                if (dt.idx == TypeIndex::String)
                    impl = FunctionDictGetString::create(context);
#define DISPATCH(TYPE) \
                else if (dt.idx == TypeIndex::TYPE) \
                    impl = FunctionDictGet<DataType##TYPE, NameDictGet##TYPE>::create(context);
                FOR_DICT_TYPES(DISPATCH)
#undef DISPATCH
                else
                    throw Exception("Unknown dictGet type", ErrorCodes::UNKNOWN_TYPE);
                return attribute.type;
            }
        }
        throw Exception{"No such attribute '" + attr_name + "'", ErrorCodes::BAD_ARGUMENTS};
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        impl->executeImpl(block, arguments, result, input_rows_count);
    }

private:
    const ExternalDictionaries & dictionaries;
    const Context & context;
    mutable FunctionPtr impl; // underlying function used by dictGet function without explicit type info
};


class FunctionDictGetNoTypeOrDefault final : public IFunction
{
public:
    static constexpr auto name = "dictGetOrDefault";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetNoTypeOrDefault>(context.getExternalDictionaries(), context);
    }

    FunctionDictGetNoTypeOrDefault(const ExternalDictionaries & dictionaries, const Context & context) : dictionaries(dictionaries), context(context) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 4; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0, 1}; }

    bool isInjective(const Block & sample_block) override
    {
        return isDictGetFunctionInjective(dictionaries, sample_block);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        String dict_name;
        if (auto name_col = checkAndGetColumnConst<ColumnString>(arguments[0].column.get()))
        {
            dict_name = name_col->getValue<String>();
        }
        else
            throw Exception{"Illegal type " + arguments[0].type->getName() + " of first argument of function " + getName()
                + ", expected a const string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        String attr_name;
        if (auto name_col = checkAndGetColumnConst<ColumnString>(arguments[1].column.get()))
        {
            attr_name = name_col->getValue<String>();
        }
        else
            throw Exception{"Illegal type " + arguments[1].type->getName() + " of second argument of function " + getName()
                + ", expected a const string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2].type).isUInt64() &&
            !isTuple(arguments[2].type))
            throw Exception{"Illegal type " + arguments[2].type->getName() + " of third argument of function " + getName()
                + ", must be UInt64 or tuple(...).", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        auto dict = dictionaries.getDictionary(dict_name);
        const DictionaryStructure & structure = dict->getStructure();

        for (const auto idx : ext::range(0, structure.attributes.size()))
        {
            const DictionaryAttribute & attribute = structure.attributes[idx];
            if (attribute.name == attr_name)
            {
                WhichDataType dt = attribute.type;
                if (dt.idx == TypeIndex::String)
                {
                    if (!isString(arguments[3].type))
                        throw Exception{"Illegal type " + arguments[3].type->getName() + " of fourth argument of function " + getName() +
                            ", must be String.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
                    impl = FunctionDictGetStringOrDefault::create(context);
                }
#define DISPATCH(TYPE) \
                else if (dt.idx == TypeIndex::TYPE) \
                { \
                    if (!checkAndGetDataType<DataType##TYPE>(arguments[3].type.get())) \
                        throw Exception{"Illegal type " + arguments[3].type->getName() + " of fourth argument of function " + getName() \
                            + ", must be " + String(DataType##TYPE{}.getFamilyName()) + ".", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT}; \
                    impl = FunctionDictGetOrDefault<DataType##TYPE, NameDictGet##TYPE ## OrDefault>::create(context); \
                }
                FOR_DICT_TYPES(DISPATCH)
#undef DISPATCH
                else
                    throw Exception("Unknown dictGetOrDefault type", ErrorCodes::UNKNOWN_TYPE);
                return attribute.type;
            }
        }
        throw Exception{"No such attribute '" + attr_name + "'", ErrorCodes::BAD_ARGUMENTS};
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        impl->executeImpl(block, arguments, result, input_rows_count);
    }

private:
    const ExternalDictionaries & dictionaries;
    const Context & context;
    mutable FunctionPtr impl; // underlying function used by dictGet function without explicit type info
};

/// Functions to work with hierarchies.

class FunctionDictGetHierarchy final : public IFunction
{
public:
    static constexpr auto name = "dictGetHierarchy";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictGetHierarchy>(context.getExternalDictionaries());
    }

    FunctionDictGetHierarchy(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 2; }
    bool isInjective(const Block & /*sample_block*/) override { return true; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[1]).isUInt64())
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", must be UInt64.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt64>());
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr) &&
            !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(Block & block, const ColumnNumbers & arguments, const size_t result,
        const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (!dict->hasHierarchy())
            throw Exception{"Dictionary does not have a hierarchy", ErrorCodes::UNSUPPORTED_METHOD};

        const auto get_hierarchies = [&] (const PaddedPODArray<UInt64> & in, PaddedPODArray<UInt64> & out, PaddedPODArray<UInt64> & offsets)
        {
            const auto size = in.size();

            /// copy of `in` array
            auto in_array = std::make_unique<PaddedPODArray<UInt64>>(std::begin(in), std::end(in));
            /// used for storing and handling result of ::toParent call
            auto out_array = std::make_unique<PaddedPODArray<UInt64>>(size);
            /// resulting hierarchies
            std::vector<std::vector<IDictionary::Key>> hierarchies(size);    /// TODO Bad code, poor performance.

            /// total number of non-zero elements, used for allocating all the required memory upfront
            size_t total_count = 0;

            while (true)
            {
                auto all_zeroes = true;

                /// erase zeroed identifiers, store non-zeroed ones
                for (const auto i : ext::range(0, size))
                {
                    const auto id = (*in_array)[i];
                    if (0 == id)
                        continue;


                    auto & hierarchy = hierarchies[i];

                    /// Checking for loop
                    if (std::find(std::begin(hierarchy), std::end(hierarchy), id) != std::end(hierarchy))
                        continue;

                    all_zeroes = false;
                    /// place id at it's corresponding place
                    hierarchy.push_back(id);

                    ++total_count;
                }

                if (all_zeroes)
                    break;

                /// translate all non-zero identifiers at once
                dict->toParent(*in_array, *out_array);

                /// we're going to use the `in_array` from this iteration as `out_array` on the next one
                std::swap(in_array, out_array);
            }

            out.reserve(total_count);
            offsets.resize(size);

            for (const auto i : ext::range(0, size))
            {
                const auto & ids = hierarchies[i];
                out.insert_assume_reserved(std::begin(ids), std::end(ids));
                offsets[i] = out.size();
            }
        };

        const auto id_col_untyped = block.getByPosition(arguments[1]).column.get();
        if (const auto id_col = checkAndGetColumn<ColumnUInt64>(id_col_untyped))
        {
            const auto & in = id_col->getData();
            auto backend = ColumnUInt64::create();
            auto offsets = ColumnArray::ColumnOffsets::create();
            get_hierarchies(in, backend->getData(), offsets->getData());
            block.getByPosition(result).column = ColumnArray::create(std::move(backend), std::move(offsets));
        }
        else if (const auto id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(id_col_untyped))
        {
            const PaddedPODArray<UInt64> in(1, id_col->getValue<UInt64>());
            auto backend = ColumnUInt64::create();
            auto offsets = ColumnArray::ColumnOffsets::create();
            get_hierarchies(in, backend->getData(), offsets->getData());
            auto array = ColumnArray::create(std::move(backend), std::move(offsets));
            block.getByPosition(result).column = block.getByPosition(result).type->createColumnConst(id_col->size(), (*array)[0].get<Array>());
        }
        else
            throw Exception{"Second argument of function " + getName() + " must be UInt64", ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    const ExternalDictionaries & dictionaries;
};


class FunctionDictIsIn final : public IFunction
{
public:
    static constexpr auto name = "dictIsIn";

    static FunctionPtr create(const Context & context)
    {
        return std::make_shared<FunctionDictIsIn>(context.getExternalDictionaries());
    }

    FunctionDictIsIn(const ExternalDictionaries & dictionaries) : dictionaries(dictionaries) {}

    String getName() const override { return name; }

private:
    size_t getNumberOfArguments() const override { return 3; }

    bool useDefaultImplementationForConstants() const final { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const final { return {0}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (!isString(arguments[0]))
            throw Exception{"Illegal type " + arguments[0]->getName() + " of first argument of function " + getName()
                + ", expected a string.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[1]).isUInt64())
            throw Exception{"Illegal type " + arguments[1]->getName() + " of second argument of function " + getName()
                + ", must be UInt64.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!WhichDataType(arguments[2]).isUInt64())
            throw Exception{"Illegal type " + arguments[2]->getName() + " of third argument of function " + getName()
                + ", must be UInt64.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataTypeUInt8>();
    }

    bool isDeterministic() const override { return false; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const auto dict_name_col = checkAndGetColumnConst<ColumnString>(block.getByPosition(arguments[0]).column.get());
        if (!dict_name_col)
            throw Exception{"First argument of function " + getName() + " must be a constant string", ErrorCodes::ILLEGAL_COLUMN};

        if (input_rows_count == 0)
        {
            auto & elem = block.getByPosition(result);
            elem.column = elem.type->createColumn();
            return;
        }

        auto dict = dictionaries.getDictionary(dict_name_col->getValue<String>());
        const auto dict_ptr = dict.get();

        if (!executeDispatch<FlatDictionary>(block, arguments, result, dict_ptr)
            && !executeDispatch<HashedDictionary>(block, arguments, result, dict_ptr)
            && !executeDispatch<CacheDictionary>(block, arguments, result, dict_ptr))
            throw Exception{"Unsupported dictionary type " + dict_ptr->getTypeName(), ErrorCodes::UNKNOWN_TYPE};
    }

    template <typename DictionaryType>
    bool executeDispatch(Block & block, const ColumnNumbers & arguments, const size_t result,
        const IDictionaryBase * dictionary)
    {
        const auto dict = typeid_cast<const DictionaryType *>(dictionary);
        if (!dict)
            return false;

        if (!dict->hasHierarchy())
            throw Exception{"Dictionary does not have a hierarchy", ErrorCodes::UNSUPPORTED_METHOD};

        const auto child_id_col_untyped = block.getByPosition(arguments[1]).column.get();
        const auto ancestor_id_col_untyped = block.getByPosition(arguments[2]).column.get();

        if (const auto child_id_col = checkAndGetColumn<ColumnUInt64>(child_id_col_untyped))
            execute(block, result, dict, child_id_col, ancestor_id_col_untyped);
        else if (const auto child_id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(child_id_col_untyped))
            execute(block, result, dict, child_id_col, ancestor_id_col_untyped);
        else
            throw Exception{"Illegal column " + child_id_col_untyped->getName()
                + " of second argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    template <typename DictionaryType>
    bool execute(Block & block, const size_t result, const DictionaryType * dictionary,
        const ColumnUInt64 * child_id_col, const IColumn * ancestor_id_col_untyped)
    {
        if (const auto ancestor_id_col = checkAndGetColumn<ColumnUInt64>(ancestor_id_col_untyped))
        {
            auto out = ColumnUInt8::create();

            const auto & child_ids = child_id_col->getData();
            const auto & ancestor_ids = ancestor_id_col->getData();
            auto & data = out->getData();
            const auto size = child_id_col->size();
            data.resize(size);

            dictionary->isInVectorVector(child_ids, ancestor_ids, data);
            block.getByPosition(result).column = std::move(out);
        }
        else if (const auto ancestor_id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(ancestor_id_col_untyped))
        {
            auto out = ColumnUInt8::create();

            const auto & child_ids = child_id_col->getData();
            const auto ancestor_id = ancestor_id_col->getValue<UInt64>();
            auto & data = out->getData();
            const auto size = child_id_col->size();
            data.resize(size);

            dictionary->isInVectorConstant(child_ids, ancestor_id, data);
            block.getByPosition(result).column = std::move(out);
        }
        else
        {
            throw Exception{"Illegal column " + ancestor_id_col_untyped->getName()
                + " of third argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN};
        }

        return true;
    }

    template <typename DictionaryType>
    bool execute(Block & block, const size_t result, const DictionaryType * dictionary,
        const ColumnConst * child_id_col, const IColumn * ancestor_id_col_untyped)
    {
        if (const auto ancestor_id_col = checkAndGetColumn<ColumnUInt64>(ancestor_id_col_untyped))
        {
            auto out = ColumnUInt8::create();

            const auto child_id = child_id_col->getValue<UInt64>();
            const auto & ancestor_ids = ancestor_id_col->getData();
            auto & data = out->getData();
            const auto size = child_id_col->size();
            data.resize(size);

            dictionary->isInConstantVector(child_id, ancestor_ids, data);
            block.getByPosition(result).column = std::move(out);
        }
        else if (const auto ancestor_id_col = checkAndGetColumnConst<ColumnVector<UInt64>>(ancestor_id_col_untyped))
        {
            const auto child_id = child_id_col->getValue<UInt64>();
            const auto ancestor_id = ancestor_id_col->getValue<UInt64>();
            UInt8 res = 0;

            dictionary->isInConstantConstant(child_id, ancestor_id, res);
            block.getByPosition(result).column = DataTypeUInt8().createColumnConst(child_id_col->size(), res);
        }
        else
            throw Exception{"Illegal column " + ancestor_id_col_untyped->getName()
                + " of third argument of function " + getName(), ErrorCodes::ILLEGAL_COLUMN};

        return true;
    }

    const ExternalDictionaries & dictionaries;
};


template <typename T>
static const PaddedPODArray<T> & getColumnDataAsPaddedPODArray(const IColumn & column, PaddedPODArray<T> & backup_storage)
{
    if (const auto vector_col = checkAndGetColumn<ColumnVector<T>>(&column))
    {
        return vector_col->getData();
    }
    if (const auto const_col = checkAndGetColumnConstData<ColumnVector<T>>(&column))
    {
        return const_col->getData();
    }

    // With type conversion, need to use backup storage here
    const auto size = column.size();
    backup_storage.resize(size);
    for (size_t i = 0; i < size; ++i)
        backup_storage[i] = column.getUInt(i);

    return backup_storage;
}

}

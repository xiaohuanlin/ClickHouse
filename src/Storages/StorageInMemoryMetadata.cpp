#include <Storages/StorageInMemoryMetadata.h>

#include <Access/AccessControl.h>
#include <Access/User.h>

#include <Core/Settings.h>

#include <Common/HashTable/HashMap.h>
#include <Common/HashTable/HashSet.h>
#include <Common/quoteString.h>
#include <Common/StringUtils.h>
#include <Core/ColumnWithTypeAndName.h>
#include <DataTypes/NestedUtils.h>
#include <DataTypes/DataTypeEnum.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExpressionActions.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/Operators.h>
#include <Parsers/ASTSQLSecurity.h>
#include <Storages/MergeTree/MergeTreeVirtualColumns.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int COLUMN_QUERIED_MORE_THAN_ONCE;
    extern const int DUPLICATE_COLUMN;
    extern const int EMPTY_LIST_OF_COLUMNS_QUERIED;
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int NOT_FOUND_COLUMN_IN_BLOCK;
    extern const int TYPE_MISMATCH;
    extern const int EMPTY_LIST_OF_COLUMNS_PASSED;
    extern const int LOGICAL_ERROR;
}

StorageInMemoryMetadata::StorageInMemoryMetadata(const StorageInMemoryMetadata & other)
    : columns(other.columns)
    , secondary_indices(other.secondary_indices)
    , constraints(other.constraints)
    , projections(other.projections.clone())
    , minmax_count_projection(
          other.minmax_count_projection ? std::optional<ProjectionDescription>(other.minmax_count_projection->clone()) : std::nullopt)
    , partition_key(other.partition_key)
    , primary_key(other.primary_key)
    , sorting_key(other.sorting_key)
    , sampling_key(other.sampling_key)
    , column_ttls_by_name(other.column_ttls_by_name)
    , table_ttl(other.table_ttl)
    , settings_changes(other.settings_changes ? other.settings_changes->clone() : nullptr)
    , select(other.select)
    , refresh(other.refresh ? other.refresh->clone() : nullptr)
    , definer(other.definer)
    , sql_security_type(other.sql_security_type)
    , comment(other.comment)
    , metadata_version(other.metadata_version)
{
}

StorageInMemoryMetadata & StorageInMemoryMetadata::operator=(const StorageInMemoryMetadata & other)
{
    if (&other == this)
        return *this;

    columns = other.columns;
    secondary_indices = other.secondary_indices;
    constraints = other.constraints;
    projections = other.projections.clone();
    if (other.minmax_count_projection)
        minmax_count_projection = other.minmax_count_projection->clone();
    else
        minmax_count_projection = std::nullopt;
    partition_key = other.partition_key;
    primary_key = other.primary_key;
    sorting_key = other.sorting_key;
    sampling_key = other.sampling_key;
    column_ttls_by_name = other.column_ttls_by_name;
    table_ttl = other.table_ttl;
    if (other.settings_changes)
        settings_changes = other.settings_changes->clone();
    else
        settings_changes.reset();
    select = other.select;
    refresh = other.refresh ? other.refresh->clone() : nullptr;
    definer = other.definer;
    sql_security_type = other.sql_security_type;
    comment = other.comment;
    metadata_version = other.metadata_version;
    return *this;
}

void StorageInMemoryMetadata::setComment(const String & comment_)
{
    comment = comment_;
}

void StorageInMemoryMetadata::setSQLSecurity(const ASTSQLSecurity & sql_security)
{
    if (sql_security.definer)
        definer = sql_security.definer->toString();
    else
        definer = std::nullopt;

    sql_security_type = sql_security.type;
}

UUID StorageInMemoryMetadata::getDefinerID(DB::ContextPtr context) const
{
    if (!definer)
    {
        if (const auto definer_id = context->getUserID())
            return *definer_id;

        throw Exception(ErrorCodes::LOGICAL_ERROR, "No user in context for sub query execution.");
    }

    const auto & access_control = context->getAccessControl();
    return access_control.getID<User>(*definer);
}

ContextMutablePtr StorageInMemoryMetadata::getSQLSecurityOverriddenContext(ContextPtr context, const ClientInfo * client_info) const
{
    if (!sql_security_type)
        return Context::createCopy(context);

    if (sql_security_type == SQLSecurityType::INVOKER)
        return Context::createCopy(context);

    auto new_context = Context::createCopy(context->getGlobalContext());
    if (client_info)
        new_context->setClientInfo(*client_info);
    else
        new_context->setClientInfo(context->getClientInfo());
    new_context->makeQueryContext();

    const auto & database = context->getCurrentDatabase();
    if (!database.empty() && database != new_context->getCurrentDatabase())
        new_context->setCurrentDatabase(database);

    new_context->setInsertionTable(context->getInsertionTable(), context->getInsertionTableColumnNames());
    new_context->setProgressCallback(context->getProgressCallback());
    new_context->setProcessListElement(context->getProcessListElement());

    if (context->getCurrentTransaction())
        new_context->setCurrentTransaction(context->getCurrentTransaction());

    if (context->getZooKeeperMetadataTransaction())
        new_context->initZooKeeperMetadataTransaction(context->getZooKeeperMetadataTransaction());

    if (sql_security_type == SQLSecurityType::NONE)
    {
        new_context->applySettingsChanges(context->getSettingsRef().changes());
        return new_context;
    }

    new_context->setUser(getDefinerID(context));

    auto changed_settings = context->getSettingsRef().changes();
    new_context->clampToSettingsConstraints(changed_settings, SettingSource::QUERY);
    new_context->applySettingsChanges(changed_settings);
    new_context->setSetting("allow_ddl", 1);

    return new_context;
}

void StorageInMemoryMetadata::setColumns(ColumnsDescription columns_)
{
    if (columns_.getAllPhysical().empty())
        throw Exception(ErrorCodes::EMPTY_LIST_OF_COLUMNS_PASSED, "Empty list of columns passed");
    columns = std::move(columns_);
}

void StorageInMemoryMetadata::setSecondaryIndices(IndicesDescription secondary_indices_)
{
    secondary_indices = std::move(secondary_indices_);
}

void StorageInMemoryMetadata::setConstraints(ConstraintsDescription constraints_)
{
    constraints = std::move(constraints_);
}

void StorageInMemoryMetadata::setProjections(ProjectionsDescription projections_)
{
    projections = std::move(projections_);
}

void StorageInMemoryMetadata::setTableTTLs(const TTLTableDescription & table_ttl_)
{
    table_ttl = table_ttl_;
}

void StorageInMemoryMetadata::setColumnTTLs(const TTLColumnsDescription & column_ttls_by_name_)
{
    column_ttls_by_name = column_ttls_by_name_;
}

void StorageInMemoryMetadata::setSettingsChanges(const ASTPtr & settings_changes_)
{
    if (settings_changes_)
        settings_changes = settings_changes_;
    else
        settings_changes = nullptr;
}

void StorageInMemoryMetadata::setSelectQuery(const SelectQueryDescription & select_)
{
    select = select_;
}

void StorageInMemoryMetadata::setRefresh(ASTPtr refresh_)
{
    refresh = refresh_;
}

void StorageInMemoryMetadata::setMetadataVersion(int32_t metadata_version_)
{
    metadata_version = metadata_version_;
}

StorageInMemoryMetadata StorageInMemoryMetadata::withMetadataVersion(int32_t metadata_version_) const
{
    StorageInMemoryMetadata copy(*this);
    copy.setMetadataVersion(metadata_version_);
    return copy;
}

const ColumnsDescription & StorageInMemoryMetadata::getColumns() const
{
    return columns;
}

const IndicesDescription & StorageInMemoryMetadata::getSecondaryIndices() const
{
    return secondary_indices;
}

bool StorageInMemoryMetadata::hasSecondaryIndices() const
{
    return !secondary_indices.empty();
}

const ConstraintsDescription & StorageInMemoryMetadata::getConstraints() const
{
    return constraints;
}

const ProjectionsDescription & StorageInMemoryMetadata::getProjections() const
{
    return projections;
}

bool StorageInMemoryMetadata::hasProjections() const
{
    return !projections.empty();
}

TTLTableDescription StorageInMemoryMetadata::getTableTTLs() const
{
    return table_ttl;
}

bool StorageInMemoryMetadata::hasAnyTableTTL() const
{
    return hasAnyMoveTTL() || hasRowsTTL() || hasAnyRecompressionTTL() || hasAnyGroupByTTL() || hasAnyRowsWhereTTL();
}

bool StorageInMemoryMetadata::hasOnlyRowsTTL() const
{
    bool has_any_other_ttl = hasAnyMoveTTL() || hasAnyRecompressionTTL() || hasAnyGroupByTTL() || hasAnyRowsWhereTTL() || hasAnyColumnTTL();
    return hasRowsTTL() && !has_any_other_ttl;
}

TTLColumnsDescription StorageInMemoryMetadata::getColumnTTLs() const
{
    return column_ttls_by_name;
}

bool StorageInMemoryMetadata::hasAnyColumnTTL() const
{
    return !column_ttls_by_name.empty();
}

TTLDescription StorageInMemoryMetadata::getRowsTTL() const
{
    return table_ttl.rows_ttl;
}

bool StorageInMemoryMetadata::hasRowsTTL() const
{
    return table_ttl.rows_ttl.expression_ast != nullptr;
}

TTLDescriptions StorageInMemoryMetadata::getRowsWhereTTLs() const
{
    return table_ttl.rows_where_ttl;
}

bool StorageInMemoryMetadata::hasAnyRowsWhereTTL() const
{
    return !table_ttl.rows_where_ttl.empty();
}

TTLDescriptions StorageInMemoryMetadata::getMoveTTLs() const
{
    return table_ttl.move_ttl;
}

bool StorageInMemoryMetadata::hasAnyMoveTTL() const
{
    return !table_ttl.move_ttl.empty();
}

TTLDescriptions StorageInMemoryMetadata::getRecompressionTTLs() const
{
    return table_ttl.recompression_ttl;
}

bool StorageInMemoryMetadata::hasAnyRecompressionTTL() const
{
    return !table_ttl.recompression_ttl.empty();
}

TTLDescriptions StorageInMemoryMetadata::getGroupByTTLs() const
{
    return table_ttl.group_by_ttl;
}

bool StorageInMemoryMetadata::hasAnyGroupByTTL() const
{
    return !table_ttl.group_by_ttl.empty();
}

ColumnDependencies StorageInMemoryMetadata::getColumnDependencies(
    const NameSet & updated_columns,
    bool include_ttl_target,
    const HasDependencyCallback & has_dependency) const
{
    if (updated_columns.empty())
        return {};

    ColumnDependencies res;

    NameSet indices_columns;
    NameSet projections_columns;
    NameSet required_ttl_columns;
    NameSet updated_ttl_columns;

    auto add_dependent_columns = [&updated_columns](const Names & required_columns, auto & to_set, bool is_projection = false)
    {
        for (const auto & dependency : required_columns)
        {
            /// useful in the case of lightweight delete with wide part and option of rebuild projection
            if (is_projection && updated_columns.contains(RowExistsColumn::name))
            {
                to_set.insert(required_columns.begin(), required_columns.end());
                return true;
            }

            if (updated_columns.contains(dependency))
            {
                to_set.insert(required_columns.begin(), required_columns.end());
                return true;
            }
        }

        return false;
    };

    for (const auto & index : getSecondaryIndices())
    {
        if (has_dependency(index.name, ColumnDependency::SKIP_INDEX))
            add_dependent_columns(index.expression->getRequiredColumns(), indices_columns);
    }

    for (const auto & projection : getProjections())
    {
        if (has_dependency(projection.name, ColumnDependency::PROJECTION))
            add_dependent_columns(projection.getRequiredColumns(), projections_columns, true);
    }

    auto add_for_rows_ttl = [&](const auto & expression, auto & to_set)
    {
        if (add_dependent_columns(expression.getNames(), to_set) && include_ttl_target)
        {
            /// Filter all columns, if rows TTL expression have to be recalculated.
            for (const auto & column : getColumns().getAllPhysical())
                updated_ttl_columns.insert(column.name);
        }
    };

    if (hasRowsTTL())
        add_for_rows_ttl(getRowsTTL().expression_columns, required_ttl_columns);

    for (const auto & entry : getRowsWhereTTLs())
        add_for_rows_ttl(entry.expression_columns, required_ttl_columns);

    for (const auto & entry : getGroupByTTLs())
        add_for_rows_ttl(entry.expression_columns, required_ttl_columns);

    for (const auto & entry : getRecompressionTTLs())
        add_dependent_columns(entry.expression_columns.getNames(), required_ttl_columns);

    for (const auto & [name, entry] : getColumnTTLs())
    {
        if (add_dependent_columns(entry.expression_columns.getNames(), required_ttl_columns) && include_ttl_target)
            updated_ttl_columns.insert(name);
    }

    for (const auto & entry : getMoveTTLs())
        add_dependent_columns(entry.expression_columns.getNames(), required_ttl_columns);

    //TODO what about rows_where_ttl and group_by_ttl ??

    for (const auto & column : indices_columns)
        res.emplace(column, ColumnDependency::SKIP_INDEX);
    for (const auto & column : projections_columns)
        res.emplace(column, ColumnDependency::PROJECTION);
    for (const auto & column : required_ttl_columns)
        res.emplace(column, ColumnDependency::TTL_EXPRESSION);
    for (const auto & column : updated_ttl_columns)
        res.emplace(column, ColumnDependency::TTL_TARGET);

    return res;
}

Block StorageInMemoryMetadata::getSampleBlockInsertable() const
{
    Block res;

    for (const auto & column : getColumns().getInsertable())
        res.insert({column.type->createColumn(), column.type, column.name});

    return res;
}

Block StorageInMemoryMetadata::getSampleBlockNonMaterialized() const
{
    Block res;

    for (const auto & column : getColumns().getOrdinary())
        res.insert({column.type->createColumn(), column.type, column.name});

    return res;
}

Block StorageInMemoryMetadata::getSampleBlockWithVirtuals(const NamesAndTypesList & virtuals) const
{
    auto res = getSampleBlock();

    /// Virtual columns must be appended after ordinary, because user can
    /// override them.
    for (const auto & column : virtuals)
        res.insert({column.type->createColumn(), column.type, column.name});

    return res;
}

Block StorageInMemoryMetadata::getSampleBlock() const
{
    Block res;

    for (const auto & column : getColumns().getAllPhysical())
        res.insert({column.type->createColumn(), column.type, column.name});

    return res;
}

Block StorageInMemoryMetadata::getSampleBlockWithSubcolumns() const
{
    Block res;

    for (const auto & column : getColumns().get(GetColumnsOptions(GetColumnsOptions::AllPhysical).withSubcolumns()))
        res.insert({column.type->createColumn(), column.type, column.name});

    return res;
}

const KeyDescription & StorageInMemoryMetadata::getPartitionKey() const
{
    return partition_key;
}

bool StorageInMemoryMetadata::isPartitionKeyDefined() const
{
    return partition_key.definition_ast != nullptr;
}

bool StorageInMemoryMetadata::hasPartitionKey() const
{
    return !partition_key.column_names.empty();
}

Names StorageInMemoryMetadata::getColumnsRequiredForPartitionKey() const
{
    if (hasPartitionKey())
        return partition_key.expression->getRequiredColumns();
    return {};
}


const KeyDescription & StorageInMemoryMetadata::getSortingKey() const
{
    return sorting_key;
}

bool StorageInMemoryMetadata::isSortingKeyDefined() const
{
    return sorting_key.definition_ast != nullptr;
}

bool StorageInMemoryMetadata::hasSortingKey() const
{
    return !sorting_key.column_names.empty();
}

Names StorageInMemoryMetadata::getColumnsRequiredForSortingKey() const
{
    if (hasSortingKey())
        return sorting_key.expression->getRequiredColumns();
    return {};
}

Names StorageInMemoryMetadata::getSortingKeyColumns() const
{
    if (hasSortingKey())
        return sorting_key.column_names;
    return {};
}

std::vector<bool> StorageInMemoryMetadata::getSortingKeyReverseFlags() const
{
    if (hasSortingKey())
        return sorting_key.reverse_flags;
    return {};
}

const KeyDescription & StorageInMemoryMetadata::getSamplingKey() const
{
    return sampling_key;
}

bool StorageInMemoryMetadata::isSamplingKeyDefined() const
{
    return sampling_key.definition_ast != nullptr;
}

bool StorageInMemoryMetadata::hasSamplingKey() const
{
    return !sampling_key.column_names.empty();
}

Names StorageInMemoryMetadata::getColumnsRequiredForSampling() const
{
    if (hasSamplingKey())
        return sampling_key.expression->getRequiredColumns();
    return {};
}

const KeyDescription & StorageInMemoryMetadata::getPrimaryKey() const
{
    return primary_key;
}

bool StorageInMemoryMetadata::isPrimaryKeyDefined() const
{
    return primary_key.definition_ast != nullptr;
}

bool StorageInMemoryMetadata::hasPrimaryKey() const
{
    return !primary_key.column_names.empty();
}

Names StorageInMemoryMetadata::getColumnsRequiredForPrimaryKey() const
{
    if (hasPrimaryKey())
        return primary_key.expression->getRequiredColumns();
    return {};
}

Names StorageInMemoryMetadata::getPrimaryKeyColumns() const
{
    if (!primary_key.column_names.empty())
        return primary_key.column_names;
    return {};
}

ASTPtr StorageInMemoryMetadata::getSettingsChanges() const
{
    if (settings_changes)
        return settings_changes->clone();
    return nullptr;
}
const SelectQueryDescription & StorageInMemoryMetadata::getSelectQuery() const
{
    return select;
}

bool StorageInMemoryMetadata::hasSelectQuery() const
{
    return select.select_query != nullptr;
}

namespace
{
    using NamesAndTypesMap = HashMapWithSavedHash<StringRef, const IDataType *, StringRefHash>;
    using UniqueStrings = HashSetWithSavedHash<StringRef, StringRefHash>;

    NamesAndTypesMap getColumnsMap(const NamesAndTypesList & columns)
    {
        NamesAndTypesMap res;

        for (const auto & column : columns)
            res.insert({column.name, column.type.get()});

        return res;
    }

    /*
     * This function checks compatibility of enums. It returns true if:
     * 1. Both types are enums.
     * 2. The first type can represent all possible values of the second one.
     * 3. Both types require the same amount of memory.
     */
    bool isCompatibleEnumTypes(const IDataType * lhs, const IDataType * rhs)
    {
        if (IDataTypeEnum const * enum_type = dynamic_cast<IDataTypeEnum const *>(lhs))
        {
            if (!enum_type->contains(*rhs))
                return false;
            return enum_type->getMaximumSizeOfValueInMemory() == rhs->getMaximumSizeOfValueInMemory();
        }
        return false;
    }
}

String listOfColumns(const NamesAndTypesList & available_columns)
{
    WriteBufferFromOwnString ss;
    for (auto it = available_columns.begin(); it != available_columns.end(); ++it)
    {
        if (it != available_columns.begin())
            ss << ", ";
        ss << it->name;
    }
    return ss.str();
}

void StorageInMemoryMetadata::check(const NamesAndTypesList & provided_columns) const
{
    const NamesAndTypesList & available_columns = getColumns().getAllPhysical();
    const auto columns_map = getColumnsMap(available_columns);

    UniqueStrings unique_names;

    for (const NameAndTypePair & column : provided_columns)
    {
        const auto * it = columns_map.find(column.name);
        if (columns_map.end() == it)
            throw Exception(
                ErrorCodes::NO_SUCH_COLUMN_IN_TABLE,
                "There is no column with name {}. There are columns: {}",
                column.name,
                listOfColumns(available_columns));

        const auto * available_type = it->getMapped();

        if (!available_type->hasDynamicSubcolumnsDeprecated()
            && !column.type->equals(*available_type)
            && !isCompatibleEnumTypes(available_type, column.type.get()))
            throw Exception(
                ErrorCodes::TYPE_MISMATCH,
                "Type mismatch for column {}. Column has type {}, got type {}",
                column.name,
                available_type->getName(),
                column.type->getName());

        if (unique_names.end() != unique_names.find(column.name))
            throw Exception(ErrorCodes::COLUMN_QUERIED_MORE_THAN_ONCE,
                "Column {} queried more than once",
                column.name);

        unique_names.insert(column.name);
    }
}

void StorageInMemoryMetadata::check(const NamesAndTypesList & provided_columns, const Names & column_names) const
{
    const NamesAndTypesList & available_columns = getColumns().getAllPhysical();
    const auto available_columns_map = getColumnsMap(available_columns);
    const auto & provided_columns_map = getColumnsMap(provided_columns);

    if (column_names.empty())
        throw Exception(ErrorCodes::EMPTY_LIST_OF_COLUMNS_QUERIED, "Empty list of columns queried. There are columns: {}",
            listOfColumns(available_columns));

    UniqueStrings unique_names;

    for (const String & name : column_names)
    {
        const auto * it = provided_columns_map.find(name);
        if (provided_columns_map.end() == it)
            continue;

        const auto * jt = available_columns_map.find(name);
        if (available_columns_map.end() == jt)
            throw Exception(
                ErrorCodes::NO_SUCH_COLUMN_IN_TABLE,
                "There is no column with name {}. There are columns: {}",
                name,
                listOfColumns(available_columns));

        const auto * provided_column_type = it->getMapped();
        const auto * available_column_type = jt->getMapped();

        if (!provided_column_type->hasDynamicSubcolumnsDeprecated()
            && !provided_column_type->equals(*available_column_type)
            && !isCompatibleEnumTypes(available_column_type, provided_column_type))
            throw Exception(
                ErrorCodes::TYPE_MISMATCH,
                "Type mismatch for column {}. Column has type {}, got type {}",
                name,
                available_column_type->getName(),
                provided_column_type->getName());

        if (unique_names.end() != unique_names.find(name))
            throw Exception(ErrorCodes::COLUMN_QUERIED_MORE_THAN_ONCE,
                "Column {} queried more than once",
                name);

        unique_names.insert(name);
    }
}

void StorageInMemoryMetadata::check(const Block & block, bool need_all) const
{
    const NamesAndTypesList & available_columns = getColumns().getAllPhysical();
    const auto columns_map = getColumnsMap(available_columns);

    NameSet names_in_block;

    block.checkNumberOfRows();

    for (const auto & column : block)
    {
        if (names_in_block.contains(column.name))
            throw Exception(ErrorCodes::DUPLICATE_COLUMN, "Duplicate column {} in block", column.name);

        names_in_block.insert(column.name);

        const auto * it = columns_map.find(column.name);
        if (columns_map.end() == it)
            throw Exception(
                ErrorCodes::NO_SUCH_COLUMN_IN_TABLE,
                "There is no column with name {}. There are columns: {}",
                column.name,
                listOfColumns(available_columns));

        const auto * available_type = it->getMapped();
        if (!available_type->hasDynamicSubcolumnsDeprecated()
            && !column.type->equals(*available_type)
            && !isCompatibleEnumTypes(available_type, column.type.get()))
            throw Exception(
                ErrorCodes::TYPE_MISMATCH,
                "Type mismatch for column {}. Column has type {}, got type {}",
                column.name,
                available_type->getName(),
                column.type->getName());
    }

    if (need_all && names_in_block.size() < columns_map.size())
    {
        for (const auto & available_column : available_columns)
        {
            if (!names_in_block.contains(available_column.name))
                throw Exception(ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK, "Expected column {}", available_column.name);
        }
    }
}


}

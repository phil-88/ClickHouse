#include <Interpreters/getHeaderForProcessingStage.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Interpreters/TreeRewriter.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Storages/IStorage.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Processors/Sources/SourceFromSingleChunk.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNSUPPORTED_METHOD;
}

bool hasJoin(const ASTSelectQuery & select)
{
    const auto & tables = select.tables();
    if (!tables || tables->children.size() < 2)
        return false;

    const auto & joined_table = tables->children[1]->as<ASTTablesInSelectQueryElement &>();
    return joined_table.table_join != nullptr;
}

/// Rewrite original query removing joined tables from it
bool removeJoin(ASTSelectQuery & select, TreeRewriterResult & rewriter_result, ContextPtr context)
{
    if (!hasJoin(select))
        return false;

    select.tables()->children.resize(1);

    /// Also remove GROUP BY cause ExpressionAnalyzer would check if it has all aggregate columns but joined columns would be missed.
    select.setExpression(ASTSelectQuery::Expression::GROUP_BY, {});
    rewriter_result.aggregates.clear();

    /// Replace select list to remove joined columns
    auto select_list = std::make_shared<ASTExpressionList>();
    for (const auto & column : rewriter_result.required_source_columns)
        select_list->children.emplace_back(std::make_shared<ASTIdentifier>(column.name));

    select.setExpression(ASTSelectQuery::Expression::SELECT, select_list);

    const DB::IdentifierMembershipCollector membership_collector{select, context};

    /// Remove unknown identifiers from where, leave only ones from left table
    auto replace_where = [&membership_collector](ASTSelectQuery & query, ASTSelectQuery::Expression expr)
    {
        auto where = query.getExpression(expr, false);
        if (!where)
            return;

        const size_t left_table_pos = 0;
        /// Test each argument of `and` function and select ones related to only left table
        std::shared_ptr<ASTFunction> new_conj = makeASTFunction("and");
        for (auto && node : splitConjunctionsAst(where))
        {
            if (membership_collector.getIdentsMembership(node) == left_table_pos)
                new_conj->arguments->children.push_back(std::move(node));
        }

        if (new_conj->arguments->children.empty())
            /// No identifiers from left table
            query.setExpression(expr, {});
        else if (new_conj->arguments->children.size() == 1)
            /// Only one expression, lift from `and`
            query.setExpression(expr, std::move(new_conj->arguments->children[0]));
        else
            /// Set new expression
            query.setExpression(expr, std::move(new_conj));
    };
    replace_where(select, ASTSelectQuery::Expression::WHERE);
    replace_where(select, ASTSelectQuery::Expression::PREWHERE);
    select.setExpression(ASTSelectQuery::Expression::HAVING, {});
    select.setExpression(ASTSelectQuery::Expression::ORDER_BY, {});

    return true;
}

class StorageDummy : public IStorage
{
public:
    StorageDummy(const StorageID & table_id_, const ColumnsDescription & columns_)
        : IStorage(table_id_)
    {
        StorageInMemoryMetadata storage_metadata;
        storage_metadata.setColumns(columns_);
        setInMemoryMetadata(storage_metadata);
    }

    std::string getName() const override { return "StorageDummy"; }

    bool supportsSampling() const override { return true; }
    bool supportsFinal() const override { return true; }
    bool supportsPrewhere() const override { return true; }
    bool supportsSubcolumns() const override { return true; }
    bool supportsDynamicSubcolumns() const override { return true; }
    bool canMoveConditionsToPrewhere() const override { return false; }

    QueryProcessingStage::Enum
    getQueryProcessingStage(ContextPtr, QueryProcessingStage::Enum, const StorageSnapshotPtr &, SelectQueryInfo &) const override
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "StorageDummy does not support getQueryProcessingStage method");
    }

    Pipe read(const Names & /*column_names*/,
        const StorageSnapshotPtr & /*storage_snapshot*/,
        SelectQueryInfo & /*query_info*/,
        ContextPtr /*context*/,
        QueryProcessingStage::Enum /*processed_stage*/,
        size_t /*max_block_size*/,
        size_t /*num_streams*/) override
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "StorageDummy does not support read method");
    }
};

Block getHeaderForProcessingStage(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    const SelectQueryInfo & query_info,
    ContextPtr context,
    QueryProcessingStage::Enum processed_stage)
{
    switch (processed_stage)
    {
        case QueryProcessingStage::FetchColumns:
        {
            Block header = storage_snapshot->getSampleBlockForColumns(column_names);

            if (query_info.prewhere_info)
            {
                auto & prewhere_info = *query_info.prewhere_info;

                if (prewhere_info.row_level_filter)
                {
                    header = prewhere_info.row_level_filter->updateHeader(std::move(header));
                    header.erase(prewhere_info.row_level_column_name);
                }

                if (prewhere_info.prewhere_actions)
                    header = prewhere_info.prewhere_actions->updateHeader(std::move(header));

                if (prewhere_info.remove_prewhere_column)
                    header.erase(prewhere_info.prewhere_column_name);
            }
            return header;
        }
        case QueryProcessingStage::WithMergeableState:
        case QueryProcessingStage::Complete:
        case QueryProcessingStage::WithMergeableStateAfterAggregation:
        case QueryProcessingStage::WithMergeableStateAfterAggregationAndLimit:
        case QueryProcessingStage::MAX:
        {
            ASTPtr query = query_info.query;
            if (const auto * select = query_info.query->as<ASTSelectQuery>(); select && hasJoin(*select))
            {
                /// TODO: Analyzer syntax analyzer result
                if (!query_info.syntax_analyzer_result)
                    throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "getHeaderForProcessingStage is unsupported");

                query = query_info.query->clone();
                TreeRewriterResult new_rewriter_result = *query_info.syntax_analyzer_result;
                removeJoin(*query->as<ASTSelectQuery>(), new_rewriter_result, context);
            }

            Block result;

            if (context->getSettingsRef().allow_experimental_analyzer)
            {
                auto storage = std::make_shared<StorageDummy>(storage_snapshot->storage.getStorageID(), storage_snapshot->metadata->getColumns());
                InterpreterSelectQueryAnalyzer interpreter(query, context, storage, SelectQueryOptions(processed_stage).analyze());
                result = interpreter.getSampleBlock();
            }
            else
            {
                auto pipe = Pipe(std::make_shared<SourceFromSingleChunk>(
                        storage_snapshot->getSampleBlockForColumns(column_names)));
                result = InterpreterSelectQuery(query, context, std::move(pipe), SelectQueryOptions(processed_stage).analyze()).getSampleBlock();
            }

            return result;
        }
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Logical Error: unknown processed stage.");
}

}


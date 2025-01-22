#include <cinttypes>
#include <cstring>
#include <ctime>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <Client/BuzzHouse/Generator/ExternalIntegrations.h>
#include <Client/BuzzHouse/Utils/HugeInt.h>
#include <Client/BuzzHouse/Utils/UHugeInt.h>

#include <IO/copyData.h>
#include <Common/ShellCommand.h>

namespace BuzzHouse
{

bool ClickHouseIntegratedDatabase::performIntegration(
    RandomGenerator & rg,
    std::shared_ptr<SQLDatabase> db,
    const uint32_t tname,
    const bool can_shuffle,
    std::vector<ColumnPathChain> & entries)
{
    const String str_tname = getTableName(db, tname);

    if (performQuery(fmt::format("DROP TABLE IF EXISTS {};", str_tname)))
    {
        String buf;
        bool first = true;

        buf += "CREATE TABLE ";
        buf += str_tname;
        buf += "(";

        if (can_shuffle && rg.nextSmallNumber() < 7)
        {
            std::shuffle(entries.begin(), entries.end(), rg.generator);
        }
        for (const auto & entry : entries)
        {
            SQLType * tp = entry.getBottomType();

            if (!first)
            {
                buf += ", ";
            }
            buf += entry.getBottomName();
            buf += " ";
            buf += columnTypeAsString(rg, tp);
            buf += " ";
            buf += ((entry.nullable.has_value() && entry.nullable.value()) || hasType<Nullable>(false, false, false, tp)) ? "" : "NOT ";
            buf += "NULL";
            assert(entry.path.size() == 1);
            first = false;
        }
        buf += ");";
        return performQuery(buf);
    }
    return false;
}

bool ClickHouseIntegratedDatabase::dropPeerTableOnRemote(const SQLTable & t)
{
    assert(t.hasDatabasePeer());
    return performQuery(fmt::format("DROP TABLE IF EXISTS {};", getTableName(t.db, t.tname)));
}

bool ClickHouseIntegratedDatabase::performCreatePeerTable(
    RandomGenerator & rg,
    const bool is_clickhouse_integration,
    const SQLTable & t,
    const CreateTable * ct,
    std::vector<ColumnPathChain> & entries)
{
    //drop table if exists in other db
    bool res = dropPeerTableOnRemote(t);

    //create table on other server
    if (res && is_clickhouse_integration)
    {
        if (t.db)
        {
            String buf;
            CreateDatabase newd;
            DatabaseEngine * deng = newd.mutable_dengine();

            newd.set_if_not_exists(true);
            deng->set_engine(t.db->deng);
            if (t.db->isReplicatedDatabase())
            {
                deng->set_zoo_path(t.db->zoo_path_counter);
            }
            newd.mutable_database()->set_database("d" + std::to_string(t.db->dname));

            CreateDatabaseToString(buf, newd);
            buf += ";";
            res &= performQuery(buf);
        }
        if (res)
        {
            String buf;
            CreateTable newt;
            newt.CopyFrom(*ct);

            assert(newt.has_est() && !newt.has_table_as());
            ExprSchemaTable & est = const_cast<ExprSchemaTable &>(newt.est());
            if (t.db)
            {
                est.mutable_database()->set_database("d" + std::to_string(t.db->dname));
            }

            CreateTableToString(buf, newt);
            buf += ";";
            res &= performQuery(buf);
        }
    }
    else if (res)
    {
        res &= performIntegration(rg, is_clickhouse_integration ? t.db : nullptr, t.tname, false, entries);
    }
    return res;
}

void ClickHouseIntegratedDatabase::truncatePeerTableOnRemote(const SQLTable & t)
{
    assert(t.hasDatabasePeer());
    auto u = performQuery(fmt::format("{} {};", truncateStatement(), getTableName(t.db, t.tname)));
    UNUSED(u);
}

void ClickHouseIntegratedDatabase::performQueryOnServerOrRemote(const PeerTableDatabase pt, const String & query)
{
    switch (pt)
    {
        case PeerTableDatabase::ClickHouse:
        case PeerTableDatabase::MySQL:
        case PeerTableDatabase::PostgreSQL:
        case PeerTableDatabase::SQLite: {
            auto u = performQuery(query);
            UNUSED(u);
        }
        break;
        case PeerTableDatabase::None: {
            auto u = fc.processServerQuery(query);
            UNUSED(u);
        }
    }
}

#if defined USE_MYSQL && USE_MYSQL
std::unique_ptr<MySQLIntegration> MySQLIntegration::testAndAddMySQLConnection(
    const FuzzConfig & fcc, const ServerCredentials & scc, const bool read_log, const String & server)
{
    MYSQL * mcon = nullptr;

    if (!(mcon = mysql_init(nullptr)))
    {
        LOG_ERROR(fcc.log, "Could not initialize MySQL handle");
    }
    else if (!mysql_real_connect(
                 mcon,
                 scc.hostname.empty() ? nullptr : scc.hostname.c_str(),
                 scc.user.empty() ? nullptr : scc.user.c_str(),
                 scc.password.empty() ? nullptr : scc.password.c_str(),
                 nullptr,
                 scc.mysql_port ? scc.mysql_port : scc.port,
                 scc.unix_socket.empty() ? nullptr : scc.unix_socket.c_str(),
                 0))
    {
        LOG_ERROR(fcc.log, "{} connection error: {}", server, mysql_error(mcon));
        mysql_close(mcon);
    }
    else
    {
        std::unique_ptr<MySQLIntegration> mysql
            = std::make_unique<MySQLIntegration>(fcc, scc, server == "ClickHouse", std::make_unique<MYSQL *>(mcon));

        if (read_log
            || (mysql->performQuery("DROP DATABASE IF EXISTS " + scc.database + ";")
                && mysql->performQuery("CREATE DATABASE " + scc.database + ";")))
        {
            LOG_INFO(fcc.log, "Connected to {}", server);
            return mysql;
        }
    }
    return nullptr;
}

void MySQLIntegration::setEngineDetails(RandomGenerator & rg, const SQLBase &, const String & tname, TableEngine * te)
{
    te->add_params()->set_svalue(sc.hostname + ":" + std::to_string(sc.mysql_port ? sc.mysql_port : sc.port));
    te->add_params()->set_svalue(sc.database);
    te->add_params()->set_svalue(tname);
    te->add_params()->set_svalue(sc.user);
    te->add_params()->set_svalue(sc.password);
    if (rg.nextBool())
    {
        te->add_params()->set_num(rg.nextBool() ? 1 : 0);
    }
}

String MySQLIntegration::getTableName(std::shared_ptr<SQLDatabase> db, const uint32_t tname)
{
    const auto prefix = is_clickhouse ? (db ? fmt::format("d{}.", db->dname) : "") : "test.";
    return fmt::format("{}t{}", prefix, tname);
}

String MySQLIntegration::truncateStatement()
{
    return fmt::format("TRUNCATE{}", is_clickhouse ? " TABLE" : "");
}

void MySQLIntegration::optimizeTableForOracle(const PeerTableDatabase pt, const SQLTable & t)
{
    assert(t.hasDatabasePeer());
    if (is_clickhouse && t.isMergeTreeFamily())
    {
        performQueryOnServerOrRemote(pt, fmt::format("ALTER TABLE {} APPLY DELETED MASK;", getTableName(t.db, t.tname)));
        performQueryOnServerOrRemote(
            pt, fmt::format("OPTIMIZE TABLE {}{};", getTableName(t.db, t.tname), t.supportsFinal() ? " FINAL" : ""));
    }
}

bool MySQLIntegration::performQuery(const String & query)
{
    if (!mysql_connection || !*mysql_connection)
    {
        LOG_ERROR(fc.log, "Not connected to MySQL");
        return false;
    }
    out_file << query << std::endl;
    if (mysql_query(*mysql_connection, query.c_str()))
    {
        LOG_ERROR(fc.log, "MySQL query: {} Error: {}", query, mysql_error(*mysql_connection));
        return false;
    }
    else
    {
        MYSQL_RES * result = mysql_store_result(*mysql_connection);

        while (mysql_fetch_row(result))
            ;
        mysql_free_result(result);
    }
    return true;
}

String MySQLIntegration::columnTypeAsString(RandomGenerator & rg, SQLType * tp) const
{
    return tp->MySQLtypeName(rg, false);
}

MySQLIntegration::~MySQLIntegration()
{
    if (mysql_connection && *mysql_connection)
    {
        mysql_close(*mysql_connection);
    }
}
#else
std::unique_ptr<MySQLIntegration>
MySQLIntegration::testAndAddMySQLConnection(const FuzzConfig & fcc, const ServerCredentials &, const bool, const String &)
{
    LOG_INFO(fcc.log, "ClickHouse not compiled with MySQL connector, skipping MySQL integration");
    return nullptr;
}
#endif

#if defined USE_LIBPQXX && USE_LIBPQXX
std::unique_ptr<PostgreSQLIntegration>
PostgreSQLIntegration::testAndAddPostgreSQLIntegration(const FuzzConfig & fcc, const ServerCredentials & scc, const bool read_log)
{
    bool has_something = false;
    String connection_str;
    std::unique_ptr<pqxx::connection> pcon = nullptr;
    std::unique_ptr<PostgreSQLIntegration> psql = nullptr;

    if (!scc.unix_socket.empty() || !scc.hostname.empty())
    {
        connection_str += "host='";
        connection_str += scc.unix_socket.empty() ? scc.hostname : scc.unix_socket;
        connection_str += "'";
        has_something = true;
    }
    if (scc.port)
    {
        if (has_something)
        {
            connection_str += " ";
        }
        connection_str += "port='";
        connection_str += std::to_string(scc.port);
        connection_str += "'";
        has_something = true;
    }
    if (!scc.user.empty())
    {
        if (has_something)
        {
            connection_str += " ";
        }
        connection_str += "user='";
        connection_str += scc.user;
        connection_str += "'";
        has_something = true;
    }
    if (!scc.password.empty())
    {
        if (has_something)
        {
            connection_str += " ";
        }
        connection_str += "password='";
        connection_str += scc.password;
        connection_str += "'";
    }
    if (!scc.database.empty())
    {
        if (has_something)
        {
            connection_str += " ";
        }
        connection_str += "dbname='";
        connection_str += scc.database;
        connection_str += "'";
    }
    try
    {
        psql = std::make_unique<PostgreSQLIntegration>(fcc, scc, std::make_unique<pqxx::connection>(connection_str));
        if (read_log || (psql->performQuery("DROP SCHEMA IF EXISTS test CASCADE;") && psql->performQuery("CREATE SCHEMA test;")))
        {
            LOG_INFO(fcc.log, "Connected to PostgreSQL");
            return psql;
        }
    }
    catch (std::exception const & e)
    {
        LOG_ERROR(fcc.log, "PostgreSQL connection error: {}", e.what());
    }
    return nullptr;
}

void PostgreSQLIntegration::setEngineDetails(RandomGenerator & rg, const SQLBase &, const String & tname, TableEngine * te)
{
    te->add_params()->set_svalue(sc.hostname + ":" + std::to_string(sc.port));
    te->add_params()->set_svalue(sc.database);
    te->add_params()->set_svalue(tname);
    te->add_params()->set_svalue(sc.user);
    te->add_params()->set_svalue(sc.password);
    te->add_params()->set_svalue("test");
    if (rg.nextSmallNumber() < 4)
    {
        te->add_params()->set_svalue("ON CONFLICT DO NOTHING");
    }
}

String PostgreSQLIntegration::getTableName(std::shared_ptr<SQLDatabase>, const uint32_t tname)
{
    return "test.t" + std::to_string(tname);
}

String PostgreSQLIntegration::truncateStatement()
{
    return "TRUNCATE";
}

bool PostgreSQLIntegration::performQuery(const String & query)
{
    if (!postgres_connection)
    {
        LOG_ERROR(fc.log, "Not connected to PostgreSQL");
        return false;
    }
    try
    {
        pqxx::work w(*postgres_connection);

        out_file << query << std::endl;
        auto u = w.exec(query);
        UNUSED(u);
        w.commit();
        return true;
    }
    catch (std::exception const & e)
    {
        LOG_ERROR(fc.log, "PostgreSQL query: {} Error: {}", query, e.what());
        return false;
    }
}

String PostgreSQLIntegration::columnTypeAsString(RandomGenerator & rg, SQLType * tp) const
{
    return tp->PostgreSQLtypeName(rg, false);
}

#else
std::unique_ptr<PostgreSQLIntegration>
PostgreSQLIntegration::testAndAddPostgreSQLIntegration(const FuzzConfig & fcc, const ServerCredentials &, const bool)
{
    LOG_INFO(fcc.log, "ClickHouse not compiled with PostgreSQL connector, skipping PostgreSQL integration");
    return nullptr;
}
#endif

#if defined USE_SQLITE && USE_SQLITE
std::unique_ptr<SQLiteIntegration> SQLiteIntegration::testAndAddSQLiteIntegration(const FuzzConfig & fcc, const ServerCredentials & scc)
{
    sqlite3 * scon = nullptr;
    const std::filesystem::path spath = fcc.db_file_path / "sqlite.db";

    if (sqlite3_open(spath.c_str(), &scon) != SQLITE_OK)
    {
        if (scon)
        {
            LOG_ERROR(fcc.log, "SQLite connection error: {}", sqlite3_errmsg(scon));
            sqlite3_close(scon);
        }
        else
        {
            LOG_ERROR(fcc.log, "Could not initialize SQLite handle");
        }
        return nullptr;
    }
    else
    {
        LOG_INFO(fcc.log, "Connected to SQLite");
        return std::make_unique<SQLiteIntegration>(fcc, scc, std::make_unique<sqlite3 *>(scon), spath);
    }
}

void SQLiteIntegration::setEngineDetails(RandomGenerator &, const SQLBase &, const String & tname, TableEngine * te)
{
    te->add_params()->set_svalue(sqlite_path.generic_string());
    te->add_params()->set_svalue(tname);
}

String SQLiteIntegration::getTableName(std::shared_ptr<SQLDatabase>, const uint32_t tname)
{
    return "t" + std::to_string(tname);
}

String SQLiteIntegration::truncateStatement()
{
    return "DELETE FROM";
}

bool SQLiteIntegration::performQuery(const String & query)
{
    char * err_msg = nullptr;

    if (!sqlite_connection || !*sqlite_connection)
    {
        LOG_ERROR(fc.log, "Not connected to SQLite");
        return false;
    }
    out_file << query << std::endl;
    if (sqlite3_exec(*sqlite_connection, query.c_str(), nullptr, nullptr, &err_msg) != SQLITE_OK)
    {
        LOG_ERROR(fc.log, "SQLite query: {} Error: {}", query, err_msg);
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

String SQLiteIntegration::columnTypeAsString(RandomGenerator & rg, SQLType * tp) const
{
    return tp->SQLitetypeName(rg, false);
}

SQLiteIntegration::~SQLiteIntegration()
{
    if (sqlite_connection && *sqlite_connection)
    {
        sqlite3_close(*sqlite_connection);
    }
}
#else
std::unique_ptr<SQLiteIntegration> SQLiteIntegration::testAndAddSQLiteIntegration(const FuzzConfig & fcc, const ServerCredentials &)
{
    LOG_INFO(fcc.log, "ClickHouse not compiled with SQLite connector, skipping SQLite integration");
    return nullptr;
}
#endif

void RedisIntegration::setEngineDetails(RandomGenerator & rg, const SQLBase &, const String &, TableEngine * te)
{
    te->add_params()->set_svalue(sc.hostname + ":" + std::to_string(sc.port));
    te->add_params()->set_num(rg.nextBool() ? 0 : rg.nextLargeNumber() % 16);
    te->add_params()->set_svalue(sc.password);
    te->add_params()->set_num(rg.nextBool() ? 16 : rg.nextLargeNumber() % 33);
}

bool RedisIntegration::performIntegration(
    RandomGenerator &, std::shared_ptr<SQLDatabase>, const uint32_t, const bool, std::vector<ColumnPathChain> &)
{
    return true;
}

#if defined USE_MONGODB && USE_MONGODB
std::unique_ptr<MongoDBIntegration> MongoDBIntegration::testAndAddMongoDBIntegration(const FuzzConfig & fcc, const ServerCredentials & scc)
{
    String connection_str = "mongodb://";

    if (!scc.user.empty())
    {
        connection_str += scc.user;
        if (!scc.password.empty())
        {
            connection_str += ":";
            connection_str += scc.password;
        }
        connection_str += "@";
    }
    connection_str += scc.hostname;
    connection_str += ":";
    connection_str += std::to_string(scc.port);

    try
    {
        bool db_exists = false;
        mongocxx::client client = mongocxx::client(mongocxx::uri(std::move(connection_str)));
        auto databases = client.list_databases();

        for (const auto & db : databases)
        {
            if (db["name"].get_utf8().value == scc.database)
            {
                db_exists = true;
                break;
            }
        }

        if (db_exists)
        {
            client[scc.database].drop();
        }

        mongocxx::database db = client[scc.database];
        db.create_collection("test");

        LOG_INFO(fcc.log, "Connected to MongoDB");
        return std::make_unique<MongoDBIntegration>(fcc, scc, client, db);
    }
    catch (const std::exception & e)
    {
        LOG_ERROR(fcc.log, "MongoDB connection error: {}", e.what());
        return nullptr;
    }
}

void MongoDBIntegration::setEngineDetails(RandomGenerator &, const SQLBase &, const String & tname, TableEngine * te)
{
    te->add_params()->set_svalue(sc.hostname + ":" + std::to_string(sc.port));
    te->add_params()->set_svalue(sc.database);
    te->add_params()->set_svalue(tname);
    te->add_params()->set_svalue(sc.user);
    te->add_params()->set_svalue(sc.password);
}

template <typename T>
constexpr bool is_document = std::is_same_v<T, bsoncxx::v_noabi::builder::stream::document>;

template <typename T>
void MongoDBIntegration::documentAppendBottomType(RandomGenerator & rg, const String & cname, T & output, SQLType * tp)
{
    IntType * itp;
    DateType * dtp;
    DateTimeType * dttp;
    DecimalType * detp;
    StringType * stp;
    EnumType * etp;
    GeoType * gtp;

    if ((itp = dynamic_cast<IntType *>(tp)))
    {
        switch (itp->size)
        {
            case 8:
            case 16:
            case 32: {
                const int32_t val = rg.nextRandomInt32();

                if constexpr (is_document<T>)
                {
                    output << cname << val;
                }
                else
                {
                    output << val;
                }
            }
            break;
            case 64: {
                const int64_t val = rg.nextRandomInt64();

                if constexpr (is_document<T>)
                {
                    output << cname << val;
                }
                else
                {
                    output << val;
                }
            }
            break;
            default: {
                HugeInt val(rg.nextRandomInt64(), rg.nextRandomUInt64());

                if constexpr (is_document<T>)
                {
                    output << cname << val.toString();
                }
                else
                {
                    output << val.toString();
                }
            }
        }
    }
    else if (dynamic_cast<FloatType *>(tp))
    {
        String buf;
        const uint32_t next_option = rg.nextLargeNumber();

        if (next_option < 25)
        {
            buf += "nan";
        }
        else if (next_option < 49)
        {
            buf += "inf";
        }
        else if (next_option < 73)
        {
            buf += "0.0";
        }
        else
        {
            std::uniform_int_distribution<uint32_t> next_dist(0, 8);
            const uint32_t left = next_dist(rg.generator);
            const uint32_t right = next_dist(rg.generator);

            buf += appendDecimal(rg, left, right);
        }
        if constexpr (is_document<T>)
        {
            output << cname << buf;
        }
        else
        {
            output << buf;
        }
    }
    else if ((dtp = dynamic_cast<DateType *>(tp)))
    {
        const bsoncxx::types::b_date val(
            {std::chrono::milliseconds(rg.nextBool() ? static_cast<uint64_t>(rg.nextRandomUInt32()) : rg.nextRandomUInt64())});

        if constexpr (is_document<T>)
        {
            output << cname << val;
        }
        else
        {
            output << val;
        }
    }
    else if ((dttp = dynamic_cast<DateTimeType *>(tp)))
    {
        String buf;

        if (dttp->extended)
        {
            buf += rg.nextDateTime64();
        }
        else
        {
            buf += rg.nextDateTime();
        }
        if constexpr (is_document<T>)
        {
            output << cname << buf;
        }
        else
        {
            output << buf;
        }
    }
    else if ((detp = dynamic_cast<DecimalType *>(tp)))
    {
        String buf;
        const uint32_t right = detp->scale.value_or(0);
        const uint32_t left = detp->precision.value_or(10) - right;

        buf += appendDecimal(rg, left, right);
        if (rg.nextBool())
        {
            bsoncxx::types::b_decimal128 decimal_value(buf.c_str());

            if constexpr (is_document<T>)
            {
                output << cname << decimal_value;
            }
            else
            {
                output << decimal_value;
            }
        }
        else if constexpr (is_document<T>)
        {
            output << cname << buf;
        }
        else
        {
            output << buf;
        }
    }
    else if ((stp = dynamic_cast<StringType *>(tp)))
    {
        const uint32_t limit = stp->precision.value_or(rg.nextRandomUInt32() % 1009);

        if (rg.nextBool())
        {
            for (uint32_t i = 0; i < limit; i++)
            {
                binary_data.push_back(static_cast<char>(rg.nextRandomInt8()));
            }
            bsoncxx::types::b_binary val{
                bsoncxx::binary_sub_type::k_binary, limit, reinterpret_cast<const std::uint8_t *>(binary_data.data())};
            if constexpr (is_document<T>)
            {
                output << cname << val;
            }
            else
            {
                output << val;
            }
            binary_data.clear();
        }
        else
        {
            if constexpr (is_document<T>)
            {
                output << cname << rg.nextString("", true, limit);
            }
            else
            {
                output << rg.nextString("", true, limit);
            }
        }
    }
    else if (dynamic_cast<const BoolType *>(tp))
    {
        const bool val = rg.nextBool();

        if constexpr (is_document<T>)
        {
            output << cname << val;
        }
        else
        {
            output << val;
        }
    }
    else if ((etp = dynamic_cast<EnumType *>(tp)))
    {
        const EnumValue & nvalue = rg.pickRandomlyFromVector(etp->values);

        if constexpr (is_document<T>)
        {
            output << cname << nvalue.val;
        }
        else
        {
            output << nvalue.val;
        }
    }
    else if (dynamic_cast<UUIDType *>(tp))
    {
        if constexpr (is_document<T>)
        {
            output << cname << rg.nextUUID();
        }
        else
        {
            output << rg.nextUUID();
        }
    }
    else if (dynamic_cast<IPv4Type *>(tp))
    {
        if constexpr (is_document<T>)
        {
            output << cname << rg.nextIPv4();
        }
        else
        {
            output << rg.nextIPv4();
        }
    }
    else if (dynamic_cast<IPv6Type *>(tp))
    {
        if constexpr (is_document<T>)
        {
            output << cname << rg.nextIPv6();
        }
        else
        {
            output << rg.nextIPv6();
        }
    }
    else if (dynamic_cast<JSONType *>(tp))
    {
        std::uniform_int_distribution<int> dopt(1, 10);
        std::uniform_int_distribution<int> wopt(1, 10);

        if constexpr (is_document<T>)
        {
            output << cname << strBuildJSON(rg, dopt(rg.generator), wopt(rg.generator));
        }
        else
        {
            output << strBuildJSON(rg, dopt(rg.generator), wopt(rg.generator));
        }
    }
    else if ((gtp = dynamic_cast<GeoType *>(tp)))
    {
        if constexpr (is_document<T>)
        {
            output << cname << strAppendGeoValue(rg, gtp->geotype);
        }
        else
        {
            output << strAppendGeoValue(rg, gtp->geotype);
        }
    }
    else
    {
        assert(0);
    }
}

void MongoDBIntegration::documentAppendArray(
    RandomGenerator & rg, const String & cname, bsoncxx::builder::stream::document & document, ArrayType * at)
{
    std::uniform_int_distribution<uint64_t> nested_rows_dist(0, fc.max_nested_rows);
    const uint64_t limit = nested_rows_dist(rg.generator);
    auto array = document << cname << bsoncxx::builder::stream::open_array; // Array
    SQLType * tp = at->subtype;
    Nullable * nl;
    VariantType * vtp;
    LowCardinality * lc;

    for (uint64_t i = 0; i < limit; i++)
    {
        const uint32_t nopt = rg.nextLargeNumber();

        if (nopt < 31)
        {
            array << bsoncxx::types::b_null{}; // Null Value
        }
        else if (nopt < 41)
        {
            array << bsoncxx::oid{}; // Oid Value
        }
        else if (nopt < 46)
        {
            array << bsoncxx::types::b_maxkey{}; // Max-Key Value
        }
        else if (nopt < 51)
        {
            array << bsoncxx::types::b_minkey{}; // Min-Key Value
        }
        else if (
            dynamic_cast<IntType *>(tp) || dynamic_cast<FloatType *>(tp) || dynamic_cast<DateType *>(tp) || dynamic_cast<DateTimeType *>(tp)
            || dynamic_cast<DecimalType *>(tp) || dynamic_cast<StringType *>(tp) || dynamic_cast<const BoolType *>(tp)
            || dynamic_cast<EnumType *>(tp) || dynamic_cast<UUIDType *>(tp) || dynamic_cast<IPv4Type *>(tp) || dynamic_cast<IPv6Type *>(tp)
            || dynamic_cast<JSONType *>(tp) || dynamic_cast<GeoType *>(tp))
        {
            documentAppendBottomType<decltype(array)>(rg, "", array, at->subtype);
        }
        else if ((lc = dynamic_cast<LowCardinality *>(tp)))
        {
            if ((nl = dynamic_cast<Nullable *>(lc->subtype)))
            {
                documentAppendBottomType<decltype(array)>(rg, "", array, nl->subtype);
            }
            else
            {
                documentAppendBottomType<decltype(array)>(rg, "", array, lc->subtype);
            }
        }
        else if ((nl = dynamic_cast<Nullable *>(tp)))
        {
            documentAppendBottomType<decltype(array)>(rg, "", array, nl->subtype);
        }
        else if (dynamic_cast<ArrayType *>(tp))
        {
            array << bsoncxx::builder::stream::open_array << 1 << bsoncxx::builder::stream::close_array;
        }
        else if ((vtp = dynamic_cast<VariantType *>(tp)))
        {
            if (vtp->subtypes.empty())
            {
                array << bsoncxx::types::b_null{}; // Null Value
            }
            else
            {
                array << 1;
            }
        }
    }
    array << bsoncxx::builder::stream::close_array;
}

void MongoDBIntegration::documentAppendAnyValue(
    RandomGenerator & rg, const String & cname, bsoncxx::builder::stream::document & document, SQLType * tp)
{
    Nullable * nl;
    ArrayType * at;
    VariantType * vtp;
    LowCardinality * lc;
    const uint32_t nopt = rg.nextLargeNumber();

    if (nopt < 31)
    {
        document << cname << bsoncxx::types::b_null{}; // Null Value
    }
    else if (nopt < 41)
    {
        document << cname << bsoncxx::oid{}; // Oid Value
    }
    else if (nopt < 46)
    {
        document << cname << bsoncxx::types::b_maxkey{}; // Max-Key Value
    }
    else if (nopt < 51)
    {
        document << cname << bsoncxx::types::b_minkey{}; // Min-Key Value
    }
    else if (
        dynamic_cast<IntType *>(tp) || dynamic_cast<FloatType *>(tp) || dynamic_cast<DateType *>(tp) || dynamic_cast<DateTimeType *>(tp)
        || dynamic_cast<DecimalType *>(tp) || dynamic_cast<StringType *>(tp) || dynamic_cast<const BoolType *>(tp)
        || dynamic_cast<EnumType *>(tp) || dynamic_cast<UUIDType *>(tp) || dynamic_cast<IPv4Type *>(tp) || dynamic_cast<IPv6Type *>(tp)
        || dynamic_cast<JSONType *>(tp) || dynamic_cast<GeoType *>(tp))
    {
        documentAppendBottomType<bsoncxx::v_noabi::builder::stream::document>(rg, cname, document, tp);
    }
    else if ((lc = dynamic_cast<LowCardinality *>(tp)))
    {
        documentAppendAnyValue(rg, cname, document, lc->subtype);
    }
    else if ((nl = dynamic_cast<Nullable *>(tp)))
    {
        documentAppendAnyValue(rg, cname, document, nl->subtype);
    }
    else if ((at = dynamic_cast<ArrayType *>(tp)))
    {
        documentAppendArray(rg, cname, document, at);
    }
    else if ((vtp = dynamic_cast<VariantType *>(tp)))
    {
        if (vtp->subtypes.empty())
        {
            document << cname << bsoncxx::types::b_null{}; // Null Value
        }
        else
        {
            documentAppendAnyValue(rg, cname, document, rg.pickRandomlyFromVector(vtp->subtypes));
        }
    }
    else
    {
        assert(0);
    }
}

bool MongoDBIntegration::performIntegration(
    RandomGenerator & rg,
    std::shared_ptr<SQLDatabase>,
    const uint32_t tname,
    const bool can_shuffle,
    std::vector<ColumnPathChain> & entries)
{
    try
    {
        const bool permute = can_shuffle && rg.nextBool();
        const bool miss_cols = rg.nextBool();
        const uint32_t ndocuments = rg.nextMediumNumber();
        const String str_tname = "t" + std::to_string(tname);
        mongocxx::collection coll = database[str_tname];

        for (uint32_t j = 0; j < ndocuments; j++)
        {
            bsoncxx::builder::stream::document document{};

            if (permute && rg.nextSmallNumber() < 4)
            {
                std::shuffle(entries.begin(), entries.end(), rg.generator);
            }
            for (const auto & entry : entries)
            {
                if (miss_cols && rg.nextSmallNumber() < 4)
                {
                    //sometimes the column is missing
                    documentAppendAnyValue(rg, entry.getBottomName(), document, entry.getBottomType());
                    assert(entry.path.size() == 1);
                }
            }
            documents.push_back(document << bsoncxx::builder::stream::finalize);
        }
        out_file << str_tname << std::endl; //collection name
        for (const auto & doc : documents)
        {
            out_file << bsoncxx::to_json(doc.view()) << std::endl; // Write each JSON document on a new line
        }
        coll.insert_many(documents);
        documents.clear();
    }
    catch (const std::exception & e)
    {
        LOG_ERROR(fc.log, "MongoDB connection error: {}", e.what());
        return false;
    }
    return true;
}
#else
std::unique_ptr<MongoDBIntegration> MongoDBIntegration::testAndAddMongoDBIntegration(const FuzzConfig & fcc, const ServerCredentials &)
{
    LOG_INFO(fcc.log, "ClickHouse not compiled with MongoDB connector, skipping MongoDB integration");
    return nullptr;
}
#endif

bool MinIOIntegration::sendRequest(const String & resource)
{
    struct tm ttm;
    ssize_t nbytes = 0;
    bool created = false;
    int sock = -1;
    int error = 0;
    char buffer[1024];
    char found_ip[1024];
    const std::time_t time = std::time({});
    DB::WriteBufferFromOwnString sign_cmd;
    DB::WriteBufferFromOwnString sign_out;
    DB::WriteBufferFromOwnString sign_err;
    DB::WriteBufferFromOwnString http_request;
    struct addrinfo hints = {};
    struct addrinfo * result = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    auto u = std::sprintf(buffer, "%" PRIu32 "", sc.port);
    UNUSED(u);
    if ((error = getaddrinfo(sc.hostname.c_str(), buffer, &hints, &result)) != 0)
    {
        if (error == EAI_SYSTEM)
        {
            strerror_r(errno, buffer, sizeof(buffer));
            LOG_ERROR(fc.log, "getnameinfo error: {}", buffer);
        }
        else
        {
            LOG_ERROR(fc.log, "getnameinfo error: {}", gai_strerror(error));
        }
        return false;
    }
    /* Loop through results */
    for (const struct addrinfo * p = result; p; p = p->ai_next)
    {
        if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
        {
            strerror_r(errno, buffer, sizeof(buffer));
            LOG_ERROR(fc.log, "Could not connect: {}", buffer);
            return false;
        }
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
        {
            if ((error = getnameinfo(p->ai_addr, p->ai_addrlen, found_ip, sizeof(found_ip), nullptr, 0, NI_NUMERICHOST)) != 0)
            {
                if (error == EAI_SYSTEM)
                {
                    strerror_r(errno, buffer, sizeof(buffer));
                    LOG_ERROR(fc.log, "getnameinfo error: {}", buffer);
                }
                else
                {
                    LOG_ERROR(fc.log, "getnameinfo error: {}", gai_strerror(error));
                }
                return false;
            }
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(result);
    if (sock == -1)
    {
        strerror_r(errno, buffer, sizeof(buffer));
        LOG_ERROR(fc.log, "Could not connect: {}", buffer);
        return false;
    }
    auto * v = gmtime_r(&time, &ttm);
    auto w = std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S %z", &ttm);
    UNUSED(v);
    UNUSED(w);
    sign_cmd << R"(printf "PUT\n\napplication/octet-stream\n)" << buffer << "\\n"
             << resource << "\""
             << " | openssl sha1 -hmac " << sc.password << " -binary | base64";
    auto res = DB::ShellCommand::execute(sign_cmd.str());
    res->in.close();
    copyData(res->out, sign_out);
    copyData(res->err, sign_err);
    res->wait();
    if (!sign_err.str().empty())
    {
        close(sock);
        LOG_ERROR(fc.log, "Error while executing shell command: {}", sign_err.str());
        return false;
    }

    http_request << "PUT " << resource << " HTTP/1.1\n"
                 << "Host: " << found_ip << ":" << std::to_string(sc.port) << "\n"
                 << "Accept: */*\n"
                 << "Date: " << buffer << "\n"
                 << "Content-Type: application/octet-stream\n"
                 << "Authorization: AWS " << sc.user << ":" << sign_out.str() << "Content-Length: 0\n\n\n";

    if (send(sock, http_request.str().c_str(), http_request.str().length(), 0) != static_cast<int>(http_request.str().length()))
    {
        strerror_r(errno, buffer, sizeof(buffer));
        close(sock);
        LOG_ERROR(fc.log, "Error sending request {}: {}", http_request.str(), buffer);
        return false;
    }
    if ((nbytes = read(sock, buffer, sizeof(buffer))) > 0 && nbytes < static_cast<ssize_t>(sizeof(buffer)) && nbytes > 12
        && !(created = (std::strncmp(buffer + 9, "200", 3) == 0)))
    {
        LOG_ERROR(fc.log, "Request {} not successful: {}", http_request.str(), buffer);
    }
    close(sock);
    return created;
}

void MinIOIntegration::setEngineDetails(RandomGenerator &, const SQLBase & b, const String & tname, TableEngine * te)
{
    te->add_params()->set_svalue(
        "http://" + sc.hostname + ":" + std::to_string(sc.port) + sc.database + "/file" + tname.substr(1)
        + (b.isS3QueueEngine() ? "/*" : ""));
    te->add_params()->set_svalue(sc.user);
    te->add_params()->set_svalue(sc.password);
}

bool MinIOIntegration::performIntegration(
    RandomGenerator &, std::shared_ptr<SQLDatabase>, const uint32_t tname, const bool, std::vector<ColumnPathChain> &)
{
    return sendRequest(sc.database + "/file" + std::to_string(tname));
}

ExternalIntegrations::ExternalIntegrations(const FuzzConfig & fcc) : fc(fcc)
{
    if (fc.mysql_server.has_value())
    {
        mysql = MySQLIntegration::testAndAddMySQLConnection(fc, fc.mysql_server.value(), fc.read_log, "MySQL");
    }
    if (fc.postgresql_server.has_value())
    {
        postresql = PostgreSQLIntegration::testAndAddPostgreSQLIntegration(fc, fc.postgresql_server.value(), fc.read_log);
    }
    if (fc.sqlite_server.has_value())
    {
        sqlite = SQLiteIntegration::testAndAddSQLiteIntegration(fc, fc.sqlite_server.value());
    }
    if (fc.mongodb_server.has_value())
    {
        mongodb = MongoDBIntegration::testAndAddMongoDBIntegration(fc, fc.mongodb_server.value());
    }
    if (fc.redis_server.has_value())
    {
        redis = std::make_unique<RedisIntegration>(fc, fc.redis_server.value());
    }
    if (fc.minio_server.has_value())
    {
        minio = std::make_unique<MinIOIntegration>(fc, fc.minio_server.value());
    }
    if (fc.clickhouse_server.has_value())
    {
        clickhouse = MySQLIntegration::testAndAddMySQLConnection(fc, fc.clickhouse_server.value(), fc.read_log, "ClickHouse");
    }
}

void ExternalIntegrations::createExternalDatabaseTable(
    RandomGenerator & rg, const IntegrationCall dc, const SQLBase & b, std::vector<ColumnPathChain> & entries, TableEngine * te)
{
    const String & tname = "t" + std::to_string(b.tname);

    requires_external_call_check++;
    switch (dc)
    {
        case IntegrationCall::MySQL:
            next_calls_succeeded.push_back(mysql->performIntegration(rg, b.db, b.tname, true, entries));
            mysql->setEngineDetails(rg, b, tname, te);
            break;
        case IntegrationCall::PostgreSQL:
            next_calls_succeeded.push_back(postresql->performIntegration(rg, b.db, b.tname, true, entries));
            postresql->setEngineDetails(rg, b, tname, te);
            break;
        case IntegrationCall::SQLite:
            next_calls_succeeded.push_back(sqlite->performIntegration(rg, b.db, b.tname, true, entries));
            sqlite->setEngineDetails(rg, b, tname, te);
            break;
        case IntegrationCall::MongoDB:
            next_calls_succeeded.push_back(mongodb->performIntegration(rg, b.db, b.tname, true, entries));
            mongodb->setEngineDetails(rg, b, tname, te);
            break;
        case IntegrationCall::Redis:
            next_calls_succeeded.push_back(redis->performIntegration(rg, b.db, b.tname, true, entries));
            redis->setEngineDetails(rg, b, tname, te);
            break;
        case IntegrationCall::MinIO:
            next_calls_succeeded.push_back(minio->performIntegration(rg, b.db, b.tname, true, entries));
            minio->setEngineDetails(rg, b, tname, te);
            break;
    }
}

void ExternalIntegrations::createPeerTable(
    RandomGenerator & rg, const PeerTableDatabase pt, const SQLTable & t, const CreateTable * ct, std::vector<ColumnPathChain> & entries)
{
    requires_external_call_check++;
    switch (pt)
    {
        case PeerTableDatabase::ClickHouse:
            next_calls_succeeded.push_back(clickhouse->performCreatePeerTable(rg, true, t, ct, entries));
            break;
        case PeerTableDatabase::MySQL:
            next_calls_succeeded.push_back(mysql->performCreatePeerTable(rg, false, t, ct, entries));
            break;
        case PeerTableDatabase::PostgreSQL:
            next_calls_succeeded.push_back(postresql->performCreatePeerTable(rg, false, t, ct, entries));
            break;
        case PeerTableDatabase::SQLite:
            next_calls_succeeded.push_back(sqlite->performCreatePeerTable(rg, false, t, ct, entries));
            break;
        case PeerTableDatabase::None:
            assert(0);
            break;
    }
}

void ExternalIntegrations::truncatePeerTableOnRemote(const SQLTable & t)
{
    switch (t.peer_table)
    {
        case PeerTableDatabase::ClickHouse:
            clickhouse->truncatePeerTableOnRemote(t);
            break;
        case PeerTableDatabase::MySQL:
            mysql->truncatePeerTableOnRemote(t);
            break;
        case PeerTableDatabase::PostgreSQL:
            postresql->truncatePeerTableOnRemote(t);
            break;
        case PeerTableDatabase::SQLite:
            sqlite->truncatePeerTableOnRemote(t);
            break;
        case PeerTableDatabase::None:
            break;
    }
}

void ExternalIntegrations::optimizeTableForOracle(const PeerTableDatabase pt, const SQLTable & t)
{
    switch (t.peer_table)
    {
        case PeerTableDatabase::ClickHouse:
            clickhouse->optimizeTableForOracle(pt, t);
            break;
        case PeerTableDatabase::MySQL:
        case PeerTableDatabase::PostgreSQL:
        case PeerTableDatabase::SQLite:
        case PeerTableDatabase::None:
            break;
    }
}

void ExternalIntegrations::dropPeerTableOnRemote(const SQLTable & t)
{
    switch (t.peer_table)
    {
        case PeerTableDatabase::ClickHouse:
            clickhouse->dropPeerTableOnRemote(t);
            break;
        case PeerTableDatabase::MySQL:
            mysql->dropPeerTableOnRemote(t);
            break;
        case PeerTableDatabase::PostgreSQL:
            postresql->dropPeerTableOnRemote(t);
            break;
        case PeerTableDatabase::SQLite:
            sqlite->dropPeerTableOnRemote(t);
            break;
        case PeerTableDatabase::None:
            break;
    }
}

bool ExternalIntegrations::performQuery(const PeerTableDatabase pt, const String & query)
{
    switch (pt)
    {
        case PeerTableDatabase::ClickHouse:
            return clickhouse->performQuery(query);
        case PeerTableDatabase::MySQL:
            return mysql->performQuery(query);
        case PeerTableDatabase::PostgreSQL:
            return postresql->performQuery(query);
        case PeerTableDatabase::SQLite:
            return sqlite->performQuery(query);
        case PeerTableDatabase::None:
            return false;
    }
}

std::filesystem::path ExternalIntegrations::getDatabaseDataDir(const PeerTableDatabase pt) const
{
    switch (pt)
    {
        case PeerTableDatabase::ClickHouse:
            return clickhouse->sc.user_files_dir / "fuzz.data";
        case PeerTableDatabase::MySQL:
            return mysql->sc.user_files_dir / "fuzz.data";
        case PeerTableDatabase::PostgreSQL:
            return postresql->sc.user_files_dir / "fuzz.data";
        case PeerTableDatabase::SQLite:
            return sqlite->sc.user_files_dir / "fuzz.data";
        case PeerTableDatabase::None:
            return fc.fuzz_out;
    }
}

void ExternalIntegrations::getPerformanceMetricsForLastQuery(
    const PeerTableDatabase pt, uint64_t & query_duration_ms, uint64_t & memory_usage)
{
    String buf;
    const std::filesystem::path out_path = this->getDatabaseDataDir(pt);

    clickhouse->performQueryOnServerOrRemote(
        pt,
        fmt::format(
            "SELECT query_duration_ms, memory_usage FROM system.query_log ORDER BY event_time_microseconds DESC LIMIT 1 INTO OUTFILE '{}' "
            "TRUNCATE FORMAT TabSeparated;",
            out_path.generic_string()));

    std::ifstream infile(out_path);
    if (std::getline(infile, buf))
    {
        const auto tabchar = buf.find('\t');

        query_duration_ms = static_cast<uint64_t>(std::stoull(buf));
        memory_usage = static_cast<uint64_t>(std::stoull(buf.substr(tabchar + 1)));
    }
}

void ExternalIntegrations::setDefaultSettings(const PeerTableDatabase pt, const std::vector<String> & settings)
{
    for (const auto & entry : settings)
    {
        clickhouse->performQueryOnServerOrRemote(pt, fmt::format("SET {} = 1;", entry));
    }
}

void ExternalIntegrations::replicateSettings(const PeerTableDatabase pt)
{
    String buf;
    String replaced;
    fc.processServerQuery(fmt::format(
        "SELECT `name`, `value` FROM system.settings WHERE changed = 1 INTO OUTFILE '{}' TRUNCATE FORMAT TabSeparated;",
        fc.fuzz_out.generic_string()));

    std::ifstream infile(fc.fuzz_out);
    while (std::getline(infile, buf))
    {
        const auto tabchar = buf.find('\t');
        const auto nname = buf.substr(0, tabchar);
        const auto nvalue = buf.substr(tabchar + 1);

        replaced.resize(0);
        for (const auto & c : nvalue)
        {
            if (c == '\'' || c == '\\')
            {
                replaced += '\\';
            }
            replaced += c;
        }
        clickhouse->performQueryOnServerOrRemote(pt, fmt::format("SET {} = '{}';", nname, replaced));
        buf.resize(0);
    }
}

}

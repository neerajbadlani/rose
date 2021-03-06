// Second implementation of Concolic::Database that works for both SQLite and PostgreSQL
// The schema is not identical to that of the first implementation.
#include <sage3basic.h>
#include <Concolic/Database.h>
#ifdef ROSE_ENABLE_CONCOLIC_TESTING

#include <MemoryMap.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/regex.hpp>
#include <boost/serialization/nvp.hpp>
#include <Concolic/ConcreteExecutor.h>
#include <Concolic/LinuxTraceExecutor.h>
#include <Concolic/Specimen.h>
#include <Concolic/SystemCall.h>
#include <Concolic/TestCase.h>
#include <Concolic/TestSuite.h>
#include <ctime>
#include <fstream>

#ifdef ROSE_HAVE_SQLITE3
#include <Sawyer/DatabaseSqlite.h>
#endif

#ifdef ROSE_HAVE_LIBPQXX
#include <Sawyer/DatabasePostgresql.h>
#endif

namespace Rose {
namespace BinaryAnalysis {
namespace Concolic {

// Return the file name part of an SQLite connection URL
static boost::filesystem::path
sqliteFileName(const std::string &url) {
    boost::regex re("(\\w*)://(.+)");
    boost::smatch matched;
    if (boost::regex_match(url, matched, re)) {
        if (matched.str(1) == "sqlite" || matched.str(1) == "sqlite3") {
            return matched.str(2);
        } else {
            return boost::filesystem::path();
        }
    } else {
        return url;
    }
}

// Initialize the database schema
static void
initSchema(Sawyer::Database::Connection db) {
    db.run("drop table if exists test_suites");
    db.run("create table test_suites ("
           " created_ts varchar(32) not null,"
           " id integer primary key,"
           " name text not null unique)");

    db.run("drop table if exists specimens");
    db.run("create table specimens ("
           " id integer primary key,"
           " created_ts varchar(32) not null,"
           " name text not null,"
           " content bytea,"                            // maybe null
           " rba bytea,"                                // maybe null
           " test_suite integer not null,"
           " constraint fk_test_suite foreign key (test_suite) references test_suites (id))");

    db.run("drop table if exists test_cases");
    db.run("create table test_cases ("
           " id integer primary key,"
           " created_ts varchar(32) not null,"
           " name text not null,"
           " executor text not null,"
           " specimen integer not null,"
           " argv bytea not null,"
           " envp bytea not null,"
           " concrete_rank real,"                       // null if concrete executor not run yet
           " concrete_result bytea,"                    // null if concrete executor not run yet
           " concolic_result integer,"                  // non-null if concolic executor has been run
           " concrete_interesting integer not null default 1," // concrete results are uninteresting if present? (bool)
           " test_suite integer not null,"
           " constraint fk_specimen foreign key (specimen) references specimens (id),"
           " constraint fk_test_suite foreign key (test_suite) references test_suites (id))");

    db.run("drop table if exists system_calls");
    db.run("create table system_calls ("
           " created_ts varchar(32) not null,"
           " id integer primary key,"
           " test_suite integer not null,"
           " test_case integer not null,"               // test case for which this system call occurs
           " call_number integer not null,"             // sequence number of instruction call within test case
           " function_number integer not null,"         // system call function identification number
           " call_site integer,"                        // address at which the system call occurs
           " retval integer,"                           // system call concrete return value
           " constraint fk_test_case foreign key (test_case) references test_cases (id),"
           " constraint fk_test_suite foreign key (test_suite) references test_suites (id))");
}

static void
initTestSuite(const Database::Ptr &db) {
    if (auto id = db->connection().get<size_t>("select id from test_suites order by created_ts desc limit 1")) {
        auto ts = db->object(TestSuiteId(*id));
        db->testSuite(ts);
    } else {
        auto ts = TestSuite::instance();
        db->testSuite(ts);
    }
}

static std::string
timestamp() {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (boost::format("%04d-%02d-%02d %02d:%02d:%02d")
            % (tm.tm_year+1900) % (tm.tm_mon+1) % tm.tm_mday
            % tm.tm_hour % tm.tm_min % tm.tm_sec).str();
}

static size_t
generateId() {
    static boost::random::mt19937 prn(::time(NULL) + ::getpid());
    boost::random::uniform_int_distribution<size_t> distributor(0, 2*1000*1000*1000); // databases have low upper limits
    return distributor(prn);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Functions for copying data from the database into an object (updateObject) or from an object into the database (updateDb)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void
updateObject(const Database::Ptr &db, TestSuiteId id, const TestSuite::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    auto iter = db->connection().stmt("select name, created_ts from test_suites where id = ?id").bind("id", *id).begin();
    if (!iter)
        throw Exception("no such test_suite in database where id=" + boost::lexical_cast<std::string>(*id));
    obj->name(*iter->get<std::string>(0));
    obj->timestamp(*iter->get<std::string>(1));
}

static void
updateDb(const Database::Ptr &db, TestSuiteId id, const TestSuite::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    Sawyer::Database::Statement stmt;
    if (*db->connection().stmt("select count(*) from test_suites where id = ?id").bind("id", *id).get<size_t>()) {
        stmt = db->connection().stmt("update test_suites set name = ?name where id = ?id");
    } else {
        std::string ts = obj->timestamp().empty() ? timestamp() : obj->timestamp();
        stmt = db->connection().stmt("insert into test_suites (id, created_ts, name) values (?id, ?ts, ?name)")
               .bind("ts", ts);
    }

    // Test suites must have unique names. Try the ID if no name was specified
    if (obj->name().empty())
        obj->name("suite-" + boost::lexical_cast<std::string>(*id));
    
    stmt
        .bind("id", *id)
        .bind("name", obj->name())
        .run();
}

static void
updateObject(const Database::Ptr &db, SpecimenId id, const Specimen::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    auto iter = db->connection().stmt("select name, created_ts, content from specimens where id = ?id").bind("id", *id).begin();
    if (!iter)
        throw Exception("no such specimen in database where id=" + boost::lexical_cast<std::string>(*id));
    obj->name(iter->get<std::string>(0).orDefault());
    obj->timestamp(iter->get<std::string>(1).orDefault());
    obj->content(iter->get<Specimen::BinaryData>(2).orDefault());
}

static void
updateDb(const Database::Ptr &db, SpecimenId id, const Specimen::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    Sawyer::Database::Statement stmt;
    if (*db->connection().stmt("select count(*) from specimens where id = ?id").bind("id", *id).get<size_t>()) {
        stmt = db->connection().stmt("update specimens set name = ?name, content = ?content where id = ?id");
    } else {
        std::string ts = obj->timestamp().empty() ? timestamp() : obj->timestamp();
        stmt = db->connection().stmt("insert into specimens (id, created_ts, name, content, test_suite)"
                                     " values (?id, ?ts, ?name, ?content, ?test_suite)")
               .bind("ts", ts)
               .bind("test_suite", *db->id(db->testSuite()));
    }
    stmt
        .bind("id", *id)
        .bind("name", obj->name())
        .bind("content", obj->content())
        .run();
}

static void
updateObject(const Database::Ptr &db, TestCaseId id, const TestCase::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    //                                        0     1         2         3     4     5              6                7
    auto iter = db->connection().stmt("select name, executor, specimen, argv, envp, concrete_rank, concolic_result, created_ts,"
                                      // 8
                                      " concrete_interesting"
                                      " from test_cases"
                                      " where id = ?id"
                                      " order by created_ts").bind("id", *id).begin();
    if (!iter)
        throw Exception("no such test case in database where id=" + boost::lexical_cast<std::string>(*id));
    obj->name(*iter->get<std::string>(0));
    obj->executor(*iter->get<std::string>(1));
    obj->specimen(db->object(SpecimenId(*iter->get<size_t>(2)), Update::YES));
    obj->args(StringUtility::split('\0', *iter->get<std::string>(3)));
    obj->concreteRank(iter->get<double>(5));
    obj->concolicResult(iter->get<size_t>(6));
    obj->timestamp(*iter->get<std::string>(7));
    obj->concreteIsInteresting(*iter->get<int>(8) != 0);

    std::vector<EnvValue> env;
    for (std::string str: StringUtility::split('\0', *iter->get<std::string>(4))) {
        auto parts = StringUtility::split('=', str, 2);
        ASSERT_require(parts.size() == 2);
        env.push_back(EnvValue(parts[0], parts[1]));
    }
    obj->env(env);
}

static void
updateDb(const Database::Ptr &db, TestCaseId id, const TestCase::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    Sawyer::Database::Statement stmt;
    if (*db->connection().stmt("select count(*) from test_cases where id = ?id").bind("id", *id).get<size_t>()) {
        stmt = db->connection().stmt("update test_cases set id = ?id, name = ?name, executor = ?executor,"
                                     " specimen = ?specimen, argv = ?argv, envp = ?envp, concrete_rank = ?concrete_rank,"
                                     " concrete_interesting = ?interesting,"
                                     " concolic_result = ?concolic_result where id = ?id");
    } else {
        std::string ts = obj->timestamp().empty() ? timestamp() : obj->timestamp();
        stmt = db->connection().stmt("insert into test_cases (id, created_ts, name, executor, specimen, argv, envp,"
                                     " concrete_rank, concrete_interesting, concolic_result, test_suite)"
                                     " values (?id, ?ts, ?name, ?executor, ?specimen, ?argv, ?envp,"
                                     " ?concrete_rank, ?interesting, ?concolic_result, ?test_suite)")
               .bind("ts", ts)
               .bind("test_suite", *db->id(db->testSuite()));
    }

    // Join environment variables to form a single string
    std::string envp;
    for (auto pair: obj->env()) {
        if (!envp.empty())
            envp += '\0';
        envp += pair.first + "=" + pair.second;
    }

    stmt
        .bind("id", *id)
        .bind("name", obj->name())
        .bind("executor", obj->executor())
        .bind("specimen", *db->id(obj->specimen(), Update::YES))
        .bind("argv", StringUtility::join('\0', obj->args()))
        .bind("envp", envp)
        .bind("concrete_rank", obj->concreteRank())
        .bind("interesting", obj->concreteIsInteresting() ? 1 : 0)
        .bind("concolic_result", obj->concolicResult())
        .run();
}

static void
updateObject(const Database::Ptr &db, SystemCallId id, const SystemCall::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);

    //                                        0           1          2            3                4          5
    auto iter = db->connection().stmt("select created_ts, test_case, call_number, function_number, call_site, retval"
                                      " from system_calls"
                                      " where id = ?id"
                                      " order by created_ts").bind("id", *id).begin();
    if (!iter)
        throw Exception("no such system call in database where id=" + boost::lexical_cast<std::string>(*id));

    TestCaseId tcid(*iter->get<size_t>(1));
    TestCase::Ptr testcase = db->object(tcid);
    ASSERT_not_null(testcase);

    obj->timestamp(*iter->get<std::string>(0));
    obj->testCase(testcase);
    obj->callSequenceNumber(*iter->get<size_t>(2));
    obj->functionId(*iter->get<size_t>(3));
    obj->callSite(*iter->get<rose_addr_t>(4));
    obj->returnValue(*iter->get<int>(5));
}

static void
updateDb(const Database::Ptr &db, SystemCallId id, const SystemCall::Ptr &obj) {
    ASSERT_not_null(db);
    ASSERT_require(id);
    ASSERT_not_null(obj);
    ASSERT_not_null(obj->testCase());
    TestCaseId tcid = db->id(obj->testCase(), Update::NO);
    ASSERT_require(tcid);

    // Assign a call sequence number if there is none yet.
    if (INVALID_INDEX == obj->callSequenceNumber()) {
        size_t csn = db->connection().stmt("select count(*) from system_calls where test_case = ?tcid")
                     .bind("tcid", *tcid)
                     .get<size_t>().get();
        obj->callSequenceNumber(csn);
    }

    Sawyer::Database::Statement stmt;
    if (*db->connection().stmt("select count(*) from system_calls where id = ?id").bind("id", *id).get<size_t>()) {
        stmt = db->connection().stmt("update system_calls set id = ?id, test_case = ?tcid, call_number = ?seq "
                                     " function_number = ?function, call_site = ?va, retval = ?retval"
                                     " where id = ?id");
    } else {
        std::string ts = obj->timestamp().empty() ? timestamp() : obj->timestamp();
        stmt = db->connection().stmt("insert into system_calls (id, created_ts, test_suite, test_case, call_number,"
                                     " function_number, call_site, retval)"
                                     " values (?id, ?ts, ?tsid, ?tcid, ?seq, ?function, ?va, ?retval)")
               .bind("ts", ts)
               .bind("tsid", *db->id(db->testSuite()));
    }

    stmt
        .bind("id", *id)
        .bind("tcid", *tcid)
        .bind("seq", obj->callSequenceNumber())
        .bind("function", obj->functionId())
        .bind("va", obj->callSite())
        .bind("retval", obj->returnValue())
        .run();
}

// Helper template function for the Database::object method
template<class IdObjMap>
static typename IdObjMap::Target // object pointer
objectHelper(const Database::Ptr &db, const typename IdObjMap::Source &id, Update::Flag update, IdObjMap &objMap,
             const std::string &objectTypeName) {
    if (!id)
        throw Exception("invalid " + objectTypeName + " ID");
    auto obj = objMap.forward().getOrDefault(id);
    if (!obj) {
        obj = IdObjMap::Target::Pointee::instance();
        updateObject(db, id, obj);
        objMap.insert(id, obj);
    } else {
        updateObject(db, id, obj);
    }
    ASSERT_not_null(obj);
    return obj;
}

// Helper template function for the Database::id method
template<class IdObjMap>
static typename IdObjMap::Source // ID
idHelper(const Database::Ptr &db, const typename IdObjMap::Target &obj, Update::Flag &update, IdObjMap &objMap) {
    ASSERT_not_null(obj);
    auto id = objMap.reverse().getOrDefault(obj);
    if (id) {
        if (Update::YES == update)
            updateDb(db, id, obj);
    } else if (Update::YES == update) {
        id = generateId();
        updateDb(db, id, obj);
        objMap.insert(id, obj);
    }
    return id;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Implementations of member functions
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Database::Database() {}

Database::~Database() {}

// class method
Database::Ptr
Database::instance(const std::string &url) {
    Ptr db(new Database);

    if (boost::starts_with(url, "postgresql://") || boost::starts_with(url, "postgres://")) {
#ifdef ROSE_HAVE_LIBPQXX
#if 0 // [Robb Matzke 2020-02-21]: not ready to use yet
        db->connection_ = Sawyer::Database::Postgresql(url);
#else
        ASSERT_not_implemented("[Robb Matzke 2020-02-21]");
#endif
#else
        throw Exception("ROSE was not configured with PostgreSQL");
#endif
    }

    boost::filesystem::path fileName = sqliteFileName(url);
    if (!fileName.empty()) {
#ifdef ROSE_HAVE_SQLITE3
        if (!boost::filesystem::exists(fileName))
            throw Exception("sqlite database " + boost::lexical_cast<std::string>(fileName) + " does not exist");
        db->connection_ = Sawyer::Database::Sqlite(fileName);
#else
        throw Exception("ROSE was not configured with SQLite");
#endif
    }

    initTestSuite(db);
    return db;
}

// class method
Database::Ptr
Database::create(const std::string &url, const Sawyer::Optional<std::string> &testSuiteName) {
    Ptr db;

    if (boost::starts_with(url, "postgresql://") || boost::starts_with(url, "postgres://")) {
#ifdef ROSE_HAVE_LIBPQXX
        throw Exception("cannot create a PostgreSQL database; use external tools to do that, then open the database");
#else
        throw Exception("ROSE was not configured with PostgreSQL");
#endif
    }

    boost::filesystem::path fileName = sqliteFileName(url);
    if (!fileName.empty()) {
#ifdef ROSE_HAVE_SQLITE3
        db = Ptr(new Database);
        boost::system::error_code ec;
        boost::filesystem::remove(fileName, ec);
        db->connection_ = Sawyer::Database::Sqlite(fileName);
        initSchema(db->connection_);
#else
        throw Exception("ROSE was not configured with SQLite");
#endif
    }

    if (testSuiteName) {
        auto testSuite = TestSuite::instance(*testSuiteName);
        db->testSuiteId_ = db->id(testSuite);
    } else {
        initTestSuite(db);
    }
    return db;
}
    

// class method
Database::Ptr
Database::create(const std::string &url) {
    return create(url, Sawyer::Nothing());
}

// class method
Database::Ptr
Database::create(const std::string &url, const std::string &testSuiteName) {
    return create(url, Sawyer::Optional<std::string>(testSuiteName));
}

std::vector<TestSuiteId>
Database::testSuites() {
    std::vector<TestSuiteId> retval;
    for (auto row: connection_.stmt("select id from test_suites order by created_ts"))
        retval.push_back(TestSuiteId(*row.get<size_t>(0)));
    return retval;
}

TestSuite::Ptr
Database::testSuite() {
    return testSuiteId_ ? object(testSuiteId_) : TestSuite::Ptr();
}

TestSuiteId
Database::testSuite(const TestSuite::Ptr &ts) {
    if (ts) {
        testSuiteId_ = id(ts);
    } else {
        testSuiteId_.reset();
    }
    return testSuiteId_;
}

std::vector<SpecimenId>
Database::specimens() {
    std::vector<SpecimenId> retval;
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection_.stmt("select id from specimens where test_suite = ?tsid order by created_ts")
               .bind("tsid", *testSuiteId_);
    } else {
        stmt = connection_.stmt("select id from specimens                          order by created_ts");
    }
    for (auto row: stmt)
        retval.push_back(SpecimenId(*row.get<size_t>(0)));
    return retval;
}

std::vector<TestCaseId>
Database::testCases() {
    std::vector<TestCaseId> retval;
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection_.stmt("select id from test_cases where test_suite = ?tsid order by created_ts")
               .bind("tsid", *testSuiteId_);
    } else {
        stmt = connection_.stmt("select id from test_cases                          order by created_ts");
    }
    for (auto row: stmt)
        retval.push_back(TestCaseId(row.get<size_t>(0)));
    return retval;
}

std::vector<SystemCallId>
Database::systemCalls() {
    std::vector<SystemCallId> retval;
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection_.stmt("select id from system_calls where test_suite = ?tsid order by created_ts")
               .bind("tsid", *testSuiteId_);
    } else {
        stmt = connection_.stmt("select id from system_calls                          order by created_ts");
    }
    for (auto row: stmt)
        retval.push_back(SystemCallId(row.get<size_t>(0)));
    return retval;
}

std::vector<SystemCallId>
Database::systemCalls(TestCaseId tcid) {
    ASSERT_require(tcid);
    Sawyer::Database::Statement stmt;
    stmt = connection_.stmt("select id from system_calls where test_case = ?tcid order by created_ts")
           .bind("tcid", *tcid);
    std::vector<SystemCallId> retval;
    for (auto row: stmt)
        retval.push_back(SystemCallId(row.get<size_t>(0)));
    return retval;
}

std::vector<SystemCallId>
Database::systemCalls(TestCaseId tcid, const std::vector<SystemCall::Ptr> &syscalls) {
    ASSERT_require(tcid);
    std::vector<SystemCallId> retval;
    eraseSystemCalls(tcid);
    for (size_t i = 0; i < syscalls.size(); ++i) {
        ASSERT_not_null(syscalls[i]);
        ASSERT_require(syscalls[i]->callSequenceNumber() == i);
        if (syscalls[i]->testCase()) {
            ASSERT_require(id(syscalls[i]->testCase(), Update::NO).isEqual(tcid));
        } else {
            syscalls[i]->testCase(object(tcid));
        }
        retval.push_back(id(syscalls[i]));
    }
    return retval;
}

size_t
Database::nSystemCalls(TestCaseId tcid) {
    ASSERT_require(tcid);
    return connection_.stmt("select count(*) from system_calls where test_case = ?tcid")
        .bind("tcid", *tcid)
        .get<size_t>().get();
}

SystemCallId
Database::systemCall(TestCaseId tcid, size_t idx) {
    ASSERT_require(tcid);
    auto stmt = connection_.stmt("select id from system_calls where test_case = ?tcid and call_number = ?idx")
                .bind("tcid", *tcid)
                .bind("idx", idx);
    for (auto row: stmt)
        return SystemCallId(row.get<size_t>(0));
    throw Exception("invalid system call index " + boost::lexical_cast<std::string>(idx) +
                    " for test case " + boost::lexical_cast<std::string>(*tcid));
}

void
Database::eraseSystemCalls(TestCaseId tcid) {
    ASSERT_require(tcid);
    connection_.stmt("delete from system_calls where test_case = ?tcid").bind("tcid", *tcid).run();
}

TestSuite::Ptr
Database::object(TestSuiteId id, Update::Flag update) {
    return objectHelper(sharedFromThis(), id, update, testSuites_, "test suite");
}

TestCase::Ptr
Database::object(TestCaseId id, Update::Flag update) {
    return objectHelper(sharedFromThis(), id, update, testCases_, "test case");
}

Specimen::Ptr
Database::object(SpecimenId id, Update::Flag update) {
    return objectHelper(sharedFromThis(), id, update, specimens_, "specimen");
}

SystemCall::Ptr
Database::object(SystemCallId id, Update::Flag update) {
    return objectHelper(sharedFromThis(), id, update, systemCalls_, "system call");
}

TestSuiteId
Database::id(const TestSuite::Ptr &obj, Update::Flag update) {
    return idHelper(sharedFromThis(), obj, update, testSuites_);
}

TestCaseId
Database::id(const TestCase::Ptr &obj, Update::Flag update) {
    return idHelper(sharedFromThis(), obj, update, testCases_);
}

SpecimenId
Database::id(const Specimen::Ptr &obj, Update::Flag update) {
    return idHelper(sharedFromThis(), obj, update, specimens_);
}

SystemCallId
Database::id(const SystemCall::Ptr &obj, Update::Flag update) {
    return idHelper(sharedFromThis(), obj, update, systemCalls_);
}

TestSuite::Ptr
Database::findTestSuite(const std::string &nameOrId) {
    if (auto id = connection_.stmt("select id from test_suites where name = ?name").bind("name", nameOrId).get<size_t>())
        return object(TestSuiteId(*id));
    try {
        TestSuiteId id(nameOrId);
        return object(id);
    } catch (...) {
        return TestSuite::Ptr();
    }
}

std::vector<SpecimenId>
Database::findSpecimensByName(const std::string &name) {
    std::vector<SpecimenId> retval;
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection_.stmt("select id from specimens where name = ?name and test_suite = ?ts").bind("ts", *testSuiteId_);
    } else {
        stmt = connection_.stmt("select id from specimens where name = ?name");
    }
    for (auto row: stmt.bind("name", name))
        retval.push_back(SpecimenId(row.get<size_t>(0)));
    return retval;
}

bool
Database::rbaExists(SpecimenId id) {
    return connection_.stmt("select count(*) from specimens where id = ?id and rba is not null")
        .bind("id", *id).get<size_t>().orElse(0) != 0;
}

void
Database::saveRbaFile(const boost::filesystem::path &fileName, SpecimenId id) {
    std::ifstream in(fileName.string().c_str(), std::ios_base::binary);
    if (!in)
        throw Exception("cannot read file \"" + StringUtility::cEscape(fileName.string()) + "\"");
    using Iter = std::istreambuf_iterator<char>;
    std::vector<uint8_t> rba;
    rba.reserve(boost::filesystem::file_size(fileName));
    rba.assign(Iter(in), Iter());
    connection().stmt("update specimens set rba = ?rba where id = ?id")
        .bind("id", *id)
        .bind("rba", rba)
        .run();
}

void
Database::extractRbaFile(const boost::filesystem::path &fileName, SpecimenId id) {
    std::ofstream out(fileName.string().c_str(), std::ios_base::binary);
    if (!out)
        throw Exception("cannot create or truncate file \"" + StringUtility::cEscape(fileName.string()) + "\"");
    auto rba = connection().stmt("select rba from specimens where id = ?id").bind("id", *id).get<std::vector<uint8_t>>();
    if (!rba)
        throw Exception("no RBA data associated with specimen " + boost::lexical_cast<std::string>(*id));
    using Iter = std::ostream_iterator<uint8_t>;
    std::copy(rba.get().begin(), rba.get().end(), Iter(out));
}

void
Database::eraseRba(SpecimenId id) {
    connection().stmt("update specimens set rba = null where id = ?id").bind("id", *id).run();
}

bool
Database::concreteResultExists(TestCaseId id) {
    return connection_.stmt("select count(*) from test_cases where id = ?id and concrete_result is not null")
        .bind("id", *id).get<size_t>().orElse(0) != 0;
}

void
Database::saveConcreteResult(const TestCase::Ptr &testCase, const ConcreteExecutorResult *details) {
    ASSERT_not_null(testCase);
    TestCaseId id = save(testCase);

    if (details) {
        double rank = details->rank();
        if (rose_isnan(rank))
            rank = 0.0;
        bool interesting = details->isInteresting();

        std::stringstream ss;
        {
            boost::archive::xml_oarchive archive(ss);
            archive.register_type<LinuxTraceExecutor::Result>();
            archive <<BOOST_SERIALIZATION_NVP(details);
        }
        connection().stmt("update test_cases"
                          " set concrete_rank = ?rank, concrete_interesting = ?interesting, concrete_result = ?details"
                          " where id = ?id")
            .bind("rank", rank)
            .bind("interesting", interesting ? 1 : 0)
            .bind("details", ss.str())
            .bind("id", *id)
            .run();

        testCase->concreteRank(rank);
        testCase->concreteIsInteresting(interesting);

    } else {
        connection().stmt("update test_cases"
                          " set concrete_rank = null, concrete_interesting = 0, concrete_result = null"
                          " where id = ?id")
            .bind("id", *id)
            .run();

        testCase->concreteRank(Sawyer::Nothing());
        testCase->concreteIsInteresting(false);
    }

    save(testCase);
}

std::unique_ptr<ConcreteExecutorResult>
Database::readConcreteResult(TestCaseId id) {
    ConcreteExecutorResult *details = nullptr;
    Sawyer::Optional<std::string> bytes = connection().stmt("select concrete_result from test_cases where id = ?id")
                                          .bind("id", *id)
                                          .get<std::string>();
    if (bytes) {
        std::istringstream ss(*bytes);
        boost::archive::xml_iarchive archive(ss);
        archive.register_type<LinuxTraceExecutor::Result>();
        archive >> BOOST_SERIALIZATION_NVP(details);
    }
    return std::unique_ptr<ConcreteExecutorResult>(details);
}

void
Database::assocTestCaseWithTestSuite(TestCaseId testCase, TestSuiteId testSuite) {
    connection().stmt("update specimen set test_suite = ?ts where id = ?tc")
        .bind("tc", *testCase)
        .bind("ts", *testSuite)
        .run();
}

std::vector<TestCaseId>
Database::needConcreteTesting(size_t n) {
    n = std::min(n, (size_t)INT_MAX);                   // some databases can't handle numbers this big
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection().stmt("select id from test_cases where concrete_rank is null and test_suite = ?ts"
                                 " order by created_ts limit ?n").bind("ts", *testSuiteId_);
    } else {
        stmt = connection().stmt("select id from test_cases where concrete_rank is null"
                                 " order by created_ts limit ?n");
    }
    std::vector<TestCaseId> retval;
    for (auto row: stmt.bind("n", n))
        retval.push_back(TestCaseId(*row.get<size_t>(0)));
    return retval;
}

std::vector<TestCaseId>
Database::needConcolicTesting(size_t n) {
    n = std::min(n, (size_t)INT_MAX);                   // some databases can't handle numbers this big
    Sawyer::Database::Statement stmt;
    if (testSuiteId_) {
        stmt = connection().stmt("select id from test_cases where concolic_result is null and test_suite = ?ts"
                                 " order by created_ts limit ?n").bind("ts", *testSuiteId_);
    } else {
        stmt = connection().stmt("select id from test_cases where concolic_result is null"
                                 " order by created_ts limit ?n");
    }
        
    std::vector<TestCaseId> retval;
    for (auto row: stmt.bind("n", n))
        retval.push_back(TestCaseId(*row.get<size_t>(0)));
    return retval;
}

bool
Database::hasUntested() {
    return !needConcreteTesting(1).empty();
}

} // namespace
} // namespace
} // namespace

#endif

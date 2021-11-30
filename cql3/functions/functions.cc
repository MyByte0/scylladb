/*
 * Copyright (C) 2014-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "functions.hh"
#include "token_fct.hh"
#include "cql3/maps.hh"
#include "cql3/sets.hh"
#include "cql3/lists.hh"
#include "cql3/constants.hh"
#include "cql3/user_types.hh"
#include "cql3/type_json.hh"
#include "database.hh"
#include "types/map.hh"
#include "types/set.hh"
#include "types/list.hh"
#include "types/user.hh"
#include "concrete_types.hh"
#include "as_json_function.hh"
#include "cql3/prepare_context.hh"
#include "user_aggregate.hh"
#include "cql3/expr/expression.hh"
#include <boost/range/adaptors.hpp>

#include "error_injection_fcts.hh"

namespace std {
std::ostream& operator<<(std::ostream& os, const std::vector<data_type>& arg_types) {
    for (size_t i = 0; i < arg_types.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << arg_types[i]->as_cql3_type().to_string();
    }
    return os;
}
}

namespace cql3 {
namespace functions {

logging::logger log("cql3_fuctions");

bool abstract_function::requires_thread() const { return false; }

bool as_json_function::requires_thread() const { return false; }

thread_local std::unordered_multimap<function_name, shared_ptr<function>> functions::_declared = init();

void functions::clear_functions() noexcept {
    functions::_declared = init();
}

std::unordered_multimap<function_name, shared_ptr<function>>
functions::init() noexcept {
    // It is possible that this function will fail with a
    // std::bad_alloc causing std::unexpected to be called. Since
    // this is used during initialization, we would have to abort
    // somehow. We could add a try/catch to print a better error
    // message before aborting, but that would produce a core file
    // that has less information in it. Given how unlikely it is that
    // we will run out of memory this early, having a better core dump
    // if we do seems like a good trade-off.
    memory::scoped_critical_alloc_section dfg;

    std::unordered_multimap<function_name, shared_ptr<function>> ret;
    auto declare = [&ret] (shared_ptr<function> f) { ret.emplace(f->name(), f); };
    declare(aggregate_fcts::make_count_rows_function());
    declare(time_uuid_fcts::make_now_fct());
    declare(time_uuid_fcts::make_min_timeuuid_fct());
    declare(time_uuid_fcts::make_max_timeuuid_fct());
    declare(time_uuid_fcts::make_date_of_fct());
    declare(time_uuid_fcts::make_unix_timestamp_of_fct());
    declare(time_uuid_fcts::make_currenttimestamp_fct());
    declare(time_uuid_fcts::make_currentdate_fct());
    declare(time_uuid_fcts::make_currenttime_fct());
    declare(time_uuid_fcts::make_currenttimeuuid_fct());
    declare(time_uuid_fcts::make_timeuuidtodate_fct());
    declare(time_uuid_fcts::make_timestamptodate_fct());
    declare(time_uuid_fcts::make_timeuuidtotimestamp_fct());
    declare(time_uuid_fcts::make_datetotimestamp_fct());
    declare(time_uuid_fcts::make_timeuuidtounixtimestamp_fct());
    declare(time_uuid_fcts::make_timestamptounixtimestamp_fct());
    declare(time_uuid_fcts::make_datetounixtimestamp_fct());
    declare(make_uuid_fct());

    for (auto&& type : cql3_type::values()) {
        // Note: because text and varchar ends up being synonymous, our automatic makeToBlobFunction doesn't work
        // for varchar, so we special case it below. We also skip blob for obvious reasons.
        if (type == cql3_type::blob) {
            continue;
        }
        // counters are not supported yet
        if (type.is_counter()) {
            warn(unimplemented::cause::COUNTERS);
            continue;
        }

        declare(make_to_blob_function(type.get_type()));
        declare(make_from_blob_function(type.get_type()));
    }

    declare(make_varchar_as_blob_fct());
    declare(make_blob_as_varchar_fct());
    add_agg_functions(ret);

    declare(error_injection::make_enable_injection_function());
    declare(error_injection::make_disable_injection_function());
    declare(error_injection::make_enabled_injections_function());

    // also needed for smp:
#if 0
    MigrationManager.instance.register(new FunctionsMigrationListener());
#endif
    return ret;
}

void functions::add_function(shared_ptr<function> func) {
    if (find(func->name(), func->arg_types())) {
        throw std::logic_error(format("duplicated function {}", func));
    }
    _declared.emplace(func->name(), func);
}

template <typename F>
void functions::with_udf_iter(const function_name& name, const std::vector<data_type>& arg_types, F&& f) {
    auto i = find_iter(name, arg_types);
    if (i == _declared.end() || i->second->is_native()) {
        log.error("attempted to remove or alter non existent user defined function {}({})", name, arg_types);
        return;
    }
    f(i);
}

void functions::replace_function(shared_ptr<function> func) {
    with_udf_iter(func->name(), func->arg_types(), [func] (functions::declared_t::iterator i) {
        i->second = std::move(func);
    });
}

void functions::remove_function(const function_name& name, const std::vector<data_type>& arg_types) {
    with_udf_iter(name, arg_types, [] (functions::declared_t::iterator i) { _declared.erase(i); });
}

std::optional<function_name> functions::used_by_user_aggregate(const function_name& name) {
    for (const shared_ptr<function>& fptr : _declared | boost::adaptors::map_values) {
        auto aggregate = dynamic_pointer_cast<user_aggregate>(fptr);
        if (aggregate && (aggregate->sfunc().name() == name || aggregate->finalfunc().name() == name)) {
            return aggregate->name();
        }
    }
    return {};
}

lw_shared_ptr<column_specification>
functions::make_arg_spec(const sstring& receiver_ks, const sstring& receiver_cf,
        const function& fun, size_t i) {
    auto&& name = boost::lexical_cast<std::string>(fun.name());
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return make_lw_shared<column_specification>(receiver_ks,
                                   receiver_cf,
                                   ::make_shared<column_identifier>(format("arg{:d}({})", i, name), true),
                                   fun.arg_types()[i]);
}

inline
shared_ptr<function>
make_to_json_function(data_type t) {
    return make_native_scalar_function<true>("tojson", utf8_type, {t},
            [t](cql_serialization_format sf, const std::vector<bytes_opt>& parameters) -> bytes_opt {
        return utf8_type->decompose(to_json_string(*t, parameters[0]));
    });
}

inline
shared_ptr<function>
make_from_json_function(database& db, const sstring& keyspace, data_type t) {
    return make_native_scalar_function<true>("fromjson", t, {utf8_type},
            [&db, keyspace, t](cql_serialization_format sf, const std::vector<bytes_opt>& parameters) -> bytes_opt {
        try {
            rjson::value json_value = rjson::parse(utf8_type->to_string(parameters[0].value()));
            bytes_opt parsed_json_value;
            if (!json_value.IsNull()) {
                parsed_json_value.emplace(from_json_object(*t, json_value, sf));
            }
            return parsed_json_value;
        } catch(rjson::error& e) {
            throw exceptions::function_execution_exception("fromJson",
                format("Failed parsing fromJson parameter: {}", e.what()), keyspace, {t->name()});
        }
    });
}

shared_ptr<function>
functions::get(database& db,
        const sstring& keyspace,
        const function_name& name,
        const std::vector<shared_ptr<assignment_testable>>& provided_args,
        const sstring& receiver_ks,
        const sstring& receiver_cf,
        const column_specification* receiver) {

    static const function_name TOKEN_FUNCTION_NAME = function_name::native_function("token");
    static const function_name TO_JSON_FUNCTION_NAME = function_name::native_function("tojson");
    static const function_name FROM_JSON_FUNCTION_NAME = function_name::native_function("fromjson");
    static const function_name MIN_FUNCTION_NAME = function_name::native_function("min");
    static const function_name MAX_FUNCTION_NAME = function_name::native_function("max");

    if (name.has_keyspace()
                ? name == TOKEN_FUNCTION_NAME
                : name.name == TOKEN_FUNCTION_NAME.name) {
        return ::make_shared<token_fct>(db.find_schema(receiver_ks, receiver_cf));
    }

    if (name.has_keyspace()
                ? name == TO_JSON_FUNCTION_NAME
                : name.name == TO_JSON_FUNCTION_NAME.name) {
        if (provided_args.size() != 1) {
            throw exceptions::invalid_request_exception("toJson() accepts 1 argument only");
        }
        selection::selector *sp = dynamic_cast<selection::selector *>(provided_args[0].get());
        if (!sp) {
            throw exceptions::invalid_request_exception("toJson() is only valid in SELECT clause");
        }
        return make_to_json_function(sp->get_type());
    }

    if (name.has_keyspace()
                ? name == FROM_JSON_FUNCTION_NAME
                : name.name == FROM_JSON_FUNCTION_NAME.name) {
        if (provided_args.size() != 1) {
            throw exceptions::invalid_request_exception("fromJson() accepts 1 argument only");
        }
        if (!receiver) {
            throw exceptions::invalid_request_exception("fromJson() can only be called if receiver type is known");
        }
        return make_from_json_function(db, keyspace, receiver->type);
    }

    if (name.has_keyspace()
                ? name == MIN_FUNCTION_NAME
                : name.name == MIN_FUNCTION_NAME.name) {
        if (provided_args.size() != 1) {
            throw exceptions::invalid_request_exception("min() operates on 1 argument at a time");
        }
        selection::selector *sp = dynamic_cast<selection::selector*>(provided_args[0].get());
        if (!sp) {
            throw exceptions::invalid_request_exception("min() is only valid in SELECT clause");
        }
        const data_type arg_type = sp->get_type();
        if (arg_type->is_collection() || arg_type->is_tuple() || arg_type->is_user_type()) {
            // `min()' function is created on demand for arguments of compound types.
            return aggregate_fcts::make_min_dynamic_function(arg_type);
        }
    }

    if (name.has_keyspace()
                ? name == MAX_FUNCTION_NAME
                : name.name == MAX_FUNCTION_NAME.name) {
        if (provided_args.size() != 1) {
            throw exceptions::invalid_request_exception("max() operates on 1 argument at a time");
        }
        selection::selector *sp = dynamic_cast<selection::selector*>(provided_args[0].get());
        if (!sp) {
            throw exceptions::invalid_request_exception("max() is only valid in SELECT clause");
        }
        const data_type arg_type = sp->get_type();
        if (arg_type->is_collection() || arg_type->is_tuple() || arg_type->is_user_type()) {
            // `max()' function is created on demand for arguments of compound types.
            return aggregate_fcts::make_max_dynamic_function(arg_type);
        }
    }

    std::vector<shared_ptr<function>> candidates;
    auto&& add_declared = [&] (function_name fn) {
        auto&& fns = _declared.equal_range(fn);
        for (auto i = fns.first; i != fns.second; ++i) {
            candidates.push_back(i->second);
        }
    };
    if (!name.has_keyspace()) {
        // add 'SYSTEM' (native) candidates
        add_declared(name.as_native_function());
        add_declared(function_name(keyspace, name.name));
    } else {
        // function name is fully qualified (keyspace + name)
        add_declared(name);
    }

    if (candidates.empty()) {
        return {};
    }

    // Fast path if there is only one choice
    if (candidates.size() == 1) {
        auto fun = std::move(candidates[0]);
        validate_types(db, keyspace, fun, provided_args, receiver_ks, receiver_cf);
        return fun;
    }

    std::vector<shared_ptr<function>> compatibles;
    for (auto&& to_test : candidates) {
        auto r = match_arguments(db, keyspace, to_test, provided_args, receiver_ks, receiver_cf);
        switch (r) {
            case assignment_testable::test_result::EXACT_MATCH:
                // We always favor exact matches
                return to_test;
            case assignment_testable::test_result::WEAKLY_ASSIGNABLE:
                compatibles.push_back(std::move(to_test));
                break;
            default:
                ;
        };
    }

    if (compatibles.empty()) {
        throw exceptions::invalid_request_exception(
                format("Invalid call to function {}, none of its type signatures match (known type signatures: {})",
                                                        name, join(", ", candidates)));
    }

    if (compatibles.size() > 1) {
        throw exceptions::invalid_request_exception(
                format("Ambiguous call to function {} (can be matched by following signatures: {}): use type casts to disambiguate",
                    name, join(", ", compatibles)));
    }

    return std::move(compatibles[0]);
}

boost::iterator_range<functions::declared_t::iterator>
functions::find(const function_name& name) {
    assert(name.has_keyspace()); // : "function name not fully qualified";
    auto pair = _declared.equal_range(name);
    return boost::make_iterator_range(pair.first, pair.second);
}

functions::declared_t::iterator
functions::find_iter(const function_name& name, const std::vector<data_type>& arg_types) {
    auto range = find(name);
    auto i = std::find_if(range.begin(), range.end(), [&] (const std::pair<const function_name, shared_ptr<function>>& d) {
        return type_equals(d.second->arg_types(), arg_types);
    });
    if (i == range.end()) {
        return _declared.end();
    }
    return i;
}

shared_ptr<function>
functions::find(const function_name& name, const std::vector<data_type>& arg_types) {
    auto i = find_iter(name, arg_types);
    if (i != _declared.end()) {
        return i->second;
    }
    return {};
}

// This method and matchArguments are somewhat duplicate, but this method allows us to provide more precise errors in the common
// case where there is no override for a given function. This is thus probably worth the minor code duplication.
void
functions::validate_types(database& db,
                          const sstring& keyspace,
                          shared_ptr<function> fun,
                          const std::vector<shared_ptr<assignment_testable>>& provided_args,
                          const sstring& receiver_ks,
                          const sstring& receiver_cf) {
    if (provided_args.size() != fun->arg_types().size()) {
        throw exceptions::invalid_request_exception(
                format("Invalid number of arguments in call to function {}: {:d} required but {:d} provided",
                        fun->name(), fun->arg_types().size(), provided_args.size()));
    }

    for (size_t i = 0; i < provided_args.size(); ++i) {
        auto&& provided = provided_args[i];

        // If the concrete argument is a bind variables, it can have any type.
        // We'll validate the actually provided value at execution time.
        if (!provided) {
            continue;
        }

        auto&& expected = make_arg_spec(receiver_ks, receiver_cf, *fun, i);
        if (!is_assignable(provided->test_assignment(db, keyspace, *expected))) {
            throw exceptions::invalid_request_exception(
                    format("Type error: {} cannot be passed as argument {:d} of function {} of type {}",
                            provided, i, fun->name(), expected->type->as_cql3_type()));
        }
    }
}

assignment_testable::test_result
functions::match_arguments(database& db, const sstring& keyspace,
        shared_ptr<function> fun,
        const std::vector<shared_ptr<assignment_testable>>& provided_args,
        const sstring& receiver_ks,
        const sstring& receiver_cf) {
    if (provided_args.size() != fun->arg_types().size()) {
        return assignment_testable::test_result::NOT_ASSIGNABLE;
    }

    // It's an exact match if all are exact match, but is not assignable as soon as any is non assignable.
    auto res = assignment_testable::test_result::EXACT_MATCH;
    for (size_t i = 0; i < provided_args.size(); ++i) {
        auto&& provided = provided_args[i];
        if (!provided) {
            res = assignment_testable::test_result::WEAKLY_ASSIGNABLE;
            continue;
        }
        auto&& expected = make_arg_spec(receiver_ks, receiver_cf, *fun, i);
        auto arg_res = provided->test_assignment(db, keyspace, *expected);
        if (arg_res == assignment_testable::test_result::NOT_ASSIGNABLE) {
            return assignment_testable::test_result::NOT_ASSIGNABLE;
        }
        if (arg_res == assignment_testable::test_result::WEAKLY_ASSIGNABLE) {
            res = assignment_testable::test_result::WEAKLY_ASSIGNABLE;
        }
    }
    return res;
}

bool
functions::type_equals(const std::vector<data_type>& t1, const std::vector<data_type>& t2) {
    return t1 == t2;
}

}
}



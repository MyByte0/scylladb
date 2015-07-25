/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright 2015 Cloudius Systems
 *
 * Modified by Cloudius Systems
 */

#pragma once

#include "cql3/statements/schema_altering_statement.hh"
#include "cql3/statements/cf_prop_defs.hh"
#include "cql3/statements/cf_statement.hh"
#include "cql3/cql3_type.hh"

#include "service/migration_manager.hh"
#include "schema.hh"

#include "core/shared_ptr.hh"

#include <unordered_map>
#include <utility>
#include <vector>
#include <set>
#include <experimental/optional>

namespace cql3 {

namespace statements {

/** A <code>CREATE TABLE</code> parsed from a CQL query statement. */
class create_table_statement : public schema_altering_statement {
#if 0
    private AbstractType<?> defaultValidator;
#endif
    std::vector<data_type> _partition_key_types;
    std::vector<data_type> _clustering_key_types;
    std::vector<bytes> _key_aliases;
    std::vector<bytes> _column_aliases;
#if 0
    private ByteBuffer valueAlias;
#endif
    bool _use_compact_storage;

    using column_map_type =
        std::unordered_map<::shared_ptr<column_identifier>,
                           data_type,
                           shared_ptr_value_hash<column_identifier>,
                           shared_ptr_equal_by_value<column_identifier>>;
    using column_set_type =
        std::unordered_set<::shared_ptr<column_identifier>,
                           shared_ptr_value_hash<column_identifier>,
                           shared_ptr_equal_by_value<column_identifier>>;
    column_map_type _columns;
    column_set_type _static_columns;
    const ::shared_ptr<cf_prop_defs> _properties;
    const bool _if_not_exists;
public:
    create_table_statement(::shared_ptr<cf_name> name,
                           ::shared_ptr<cf_prop_defs> properties,
                           bool if_not_exists,
                           column_set_type static_columns);

    virtual void check_access(const service::client_state& state) override;

    virtual void validate(distributed<service::storage_proxy>&, const service::client_state& state) override;

    virtual future<bool> announce_migration(distributed<service::storage_proxy>& proxy, bool is_local_only) override;

    virtual shared_ptr<transport::event::schema_change> change_event() override;

    schema_ptr get_cf_meta_data();

    class raw_statement;

    friend raw_statement;
private:
    std::vector<column_definition> get_columns();

    void apply_properties_to(schema_builder& builder);

    void add_column_metadata_from_aliases(schema_builder& builder, std::vector<bytes> aliases, const std::vector<data_type>& types, column_kind kind);
};

class create_table_statement::raw_statement : public cf_statement {
private:
    using defs_type = std::unordered_map<::shared_ptr<column_identifier>,
                                         ::shared_ptr<cql3_type::raw>,
                                         shared_ptr_value_hash<column_identifier>,
                                         shared_ptr_equal_by_value<column_identifier>>;
    defs_type _definitions;
public:
    const ::shared_ptr<cf_prop_defs> properties = ::make_shared<cf_prop_defs>();
private:
    std::vector<std::vector<::shared_ptr<column_identifier>>> _key_aliases;
    std::vector<::shared_ptr<column_identifier>> _column_aliases;
    std::vector<std::pair<::shared_ptr<column_identifier>, bool>> _defined_ordering; // Insertion ordering is important
    std::experimental::optional<bool> find_ordering_info(::shared_ptr<column_identifier> type) {
        for (auto& t: _defined_ordering) {
            if (*(t.first) == *type) {
                return t.second;
            }
        }
        return {};
    }
    create_table_statement::column_set_type _static_columns;

    bool _use_compact_storage = false;
    std::multiset<::shared_ptr<column_identifier>> _defined_names;
    bool _if_not_exists;
public:
    raw_statement(::shared_ptr<cf_name> name, bool if_not_exists);

    virtual ::shared_ptr<prepared> prepare(database& db) override;

    data_type get_type_and_remove(column_map_type& columns, ::shared_ptr<column_identifier> t);

    void add_definition(::shared_ptr<column_identifier> def, ::shared_ptr<cql3_type::raw> type, bool is_static);

    void add_key_aliases(const std::vector<::shared_ptr<column_identifier>> aliases);

    void add_column_alias(::shared_ptr<column_identifier> alias);

    void set_ordering(::shared_ptr<column_identifier> alias, bool reversed);

    void set_compact_storage();
};

}

}

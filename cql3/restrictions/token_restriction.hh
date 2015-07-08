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

#include "restriction.hh"
#include "primary_key_restrictions.hh"
#include "exceptions/exceptions.hh"
#include "term_slice.hh"
#include "keys.hh"

class column_definition;

namespace cql3 {

namespace restrictions {

/**
 * <code>Restriction</code> using the token function.
 */
class token_restriction: public primary_key_restrictions<partition_key> {
private:
    /**
     * The definition of the columns to which apply the token restriction.
     */
    std::vector<const column_definition *> _column_definitions;
public:
    token_restriction(std::vector<const column_definition *> c)
            : _column_definitions(std::move(c)) {
    }

    bool is_on_token() const override {
        return true;
    }
    std::vector<const column_definition*> get_column_defs() const override {
        return _column_definitions;
    }

#if 0
    bool has_supporting_index(::shared_ptr<secondary_index_manager> index_manager) const override {
        return false;
    }

    void add_index_expression_to(std::vector<::shared_ptr<index_expression>>& expressions,
                                         const query_options& options) override {
        throw exceptions::unsupported_operation_exception();
    }
#endif

    std::vector<partition_key> values_as_keys(const query_options& options) const override {
        throw exceptions::unsupported_operation_exception();
    }

    std::vector<bounds_range_type> bounds_ranges(const query_options& options) const override {
        auto get_token_bound = [this, &options](statements::bound b) {
            if (!has_bound(b)) {
                return dht::minimum_token();
            }
            auto buf= bounds(b, options).front();
            if (!buf) {
                throw exceptions::invalid_request_exception("Invalid null token value");
            }
            return dht::token(dht::token::kind::key, *buf);
        };

        const auto start_token = get_token_bound(statements::bound::START);
        const auto end_token = get_token_bound(statements::bound::END);
        const auto include_start = this->is_inclusive(statements::bound::START);
        const auto include_end = this->is_inclusive(statements::bound::END);

        /*
         * If we ask SP.getRangeSlice() for (token(200), token(200)], it will happily return the whole ring.
         * However, wrapping range doesn't really make sense for CQL, and we want to return an empty result in that
         * case (CASSANDRA-5573). So special case to create a range that is guaranteed to be empty.
         *
         * In practice, we want to return an empty result set if either startToken > endToken, or both are equal but
         * one of the bound is excluded (since [a, a] can contains something, but not (a, a], [a, a) or (a, a)).
         * Note though that in the case where startToken or endToken is the minimum token, then this special case
         * rule should not apply.
         */
        if (start_token != dht::minimum_token()
                && end_token != dht::minimum_token()
                && (start_token > end_token
                        || (start_token == end_token
                                && (!include_start || !include_end)))) {
            return {query::partition_range::make_open_ended_both_sides()};
        }

        typedef typename bounds_range_type::bound bound;

        auto start = bound(start_token, include_start);
        auto end = bound(end_token, include_end);

        return { bounds_range_type(start, end) };
    }

    class EQ;
    class slice;
};


class token_restriction::EQ final : public token_restriction {
private:
    ::shared_ptr<term> _value;
public:
    EQ(std::vector<const column_definition*> column_defs, ::shared_ptr<term> value)
        : token_restriction(column_defs)
        , _value(std::move(value))
    {}

    bool is_EQ() const {
        return true;
    }

    bool uses_function(const sstring& ks_name, const sstring& function_name) const override {
        return abstract_restriction::uses_function(_value, ks_name, function_name);
    }

    void merge_with(::shared_ptr<restriction>) override {
        throw exceptions::invalid_request_exception(
                join(", ", get_column_defs())
                        + " cannot be restricted by more than one relation if it includes an Equal");
    }

    std::vector<bytes_opt> values(const query_options& options) const override {
        return { _value->bind_and_get(options) };
    }

    sstring to_string() const override {
        return sprint("EQ(%s)", _value->to_string());
    }
};

class token_restriction::slice final : public token_restriction {
private:
    term_slice _slice;
public:
    slice(std::vector<const column_definition*> column_defs, statements::bound bound, bool inclusive, ::shared_ptr<term> term)
        : token_restriction(column_defs)
        , _slice(term_slice::new_instance(bound, inclusive, std::move(term)))
    {}

    bool is_slice() const override {
        return true;
    }

    bool has_bound(statements::bound b) const override {
        return _slice.has_bound(b);
    }

    std::vector<bytes_opt> values(const query_options& options) const override {
        throw exceptions::unsupported_operation_exception();
    }

    std::vector<bytes_opt> bounds(statements::bound b, const query_options& options) const override {
        return { _slice.bound(b)->bind_and_get(options) };
    }

    bool uses_function(const sstring& ks_name,
            const sstring& function_name) const override {
        return (_slice.has_bound(statements::bound::START)
                && abstract_restriction::uses_function(
                        _slice.bound(statements::bound::START), ks_name,
                        function_name))
                || (_slice.has_bound(statements::bound::END)
                        && abstract_restriction::uses_function(
                                _slice.bound(statements::bound::END),
                                ks_name, function_name));
    }
    bool is_inclusive(statements::bound b) const override {
        return _slice.is_inclusive(b);
    }
    void merge_with(::shared_ptr<restriction> restriction) override {
        try {
            if (!restriction->is_on_token()) {
                throw exceptions::invalid_request_exception(
                        "Columns \"%s\" cannot be restricted by both a normal relation and a token relation");
            }
            if (!restriction->is_slice()) {
                throw exceptions::invalid_request_exception(
                        "Columns \"%s\" cannot be restricted by both an equality and an inequality relation");
            }

            auto* other_slice = static_cast<slice *>(restriction.get());

            if (has_bound(statements::bound::START)
                    && other_slice->has_bound(statements::bound::START)) {
                throw exceptions::invalid_request_exception(
                        "More than one restriction was found for the start bound on %s");
            }
            if (has_bound(statements::bound::END)
                    && other_slice->has_bound(statements::bound::END)) {
                throw exceptions::invalid_request_exception(
                        "More than one restriction was found for the end bound on %s");
            }
            _slice.merge(other_slice->_slice);
        } catch (exceptions::invalid_request_exception & e) {
            throw exceptions::invalid_request_exception(
                    sprint(e.what(), join(", ", get_column_defs())));
        }
    }
    sstring to_string() const override {
        return sprint("SLICE%s", _slice);
    }
};

}

}
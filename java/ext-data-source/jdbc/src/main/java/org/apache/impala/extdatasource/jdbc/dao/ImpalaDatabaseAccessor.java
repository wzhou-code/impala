// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.impala.extdatasource.jdbc.dao;

import java.util.ArrayList;
import java.util.List;

import com.google.common.base.Splitter;
import com.google.common.base.Strings;
import com.google.common.collect.Lists;

/**
 * Impala specific data accessor. This is needed because Impala JDBC drivers do not
 * support generic LIMIT and OFFSET escape functions
 */
public class ImpalaDatabaseAccessor extends GenericJdbcDatabaseAccessor {

  @Override
  protected String addLimitAndOffsetToQuery(String sql, int limit, int offset) {
    if (offset == 0) {
      return addLimitToQuery(sql, limit);
    } else {
      if (limit != -1) {
        return sql + " LIMIT " + limit + " OFFSET " + offset;
      } else {
        return sql;
      }
    }
  }

  @Override
  protected String addLimitToQuery(String sql, int limit) {
    if (limit != -1) {
      return sql + " LIMIT " + limit;
    } else {
      return sql;
    }
  }

  @Override
  public String getOptions(String configOptions) {
    if (Strings.isNullOrEmpty(configOptions)) return null;
    // Extract valid query options.
    List<String> options = Lists.newArrayList(Splitter.on(',').trimResults()
        .omitEmptyStrings().split(configOptions.toLowerCase()));
    List<String> invalidOptions = new ArrayList();
    for (String option : options) {
      if (!option.contains("=")) {
        invalidOptions.add(option);
      }
    }
    if (!invalidOptions.isEmpty()) {
      options.removeAll(invalidOptions);
    }
    if (!options.isEmpty()) {
      return String.join(";", options);
    } else {
      return null;
    }
  }
}

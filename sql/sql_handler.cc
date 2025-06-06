/* Copyright (c) 2001, 2015, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/* HANDLER ... commands - direct access to ISAM */

/* TODO:
  HANDLER blabla OPEN [ AS foobar ] [ (column-list) ]

  the most natural (easiest, fastest) way to do it is to
  compute List<Item> field_list not in mysql_ha_read
  but in mysql_ha_open, and then store it in TABLE structure.

  The problem here is that mysql_parse calls free_item to free all the
  items allocated at the end of every query. The workaround would to
  keep two item lists per THD - normal free_list and handler_items.
  The second is to be freed only on thread end. mysql_ha_open should
  then do { handler_items=concat(handler_items, free_list); free_list=0; }

  But !!! do_command calls free_root at the end of every query and frees up
  all the memory allocated on THD::mem_root. It's harder to work around...
*/

/*
  The information about open HANDLER objects is stored in a HASH.
  It holds objects of type TABLE_LIST, which are indexed by table
  name/alias, and allows us to quickly find a HANDLER table for any
  operation at hand - be it HANDLER READ or HANDLER CLOSE.

  It also allows us to maintain an "open" HANDLER even in cases
  when there is no physically open cursor. E.g. a FLUSH TABLE
  statement in this or some other connection demands that all open
  HANDLERs against the flushed table are closed. In order to
  preserve the information about an open HANDLER, we don't perform
  a complete HANDLER CLOSE, but only close the TABLE object.  The
  corresponding TABLE_LIST is kept in the cache with 'table'
  pointer set to NULL. The table will be reopened on next access
  (this, however, leads to loss of cursor position, unless the
  cursor points at the first record).
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_handler.h"
#include "sql_base.h"                           // close_thread_tables
#include "lock.h"                               // mysql_unlock_tables
#include "key.h"                                // key_copy
#include "sql_base.h"                           // insert_fields
#include "sql_select.h"
#include "transaction.h"

#define HANDLER_TABLES_HASH_SIZE 120

static enum enum_ha_read_modes rkey_to_rnext[]=
{ RNEXT_SAME, RNEXT, RPREV, RNEXT, RPREV, RNEXT, RPREV, RPREV };

/*
  Set handler to state after create, but keep base information about
  which table is used
*/

void SQL_HANDLER::reset()
{
  fields.empty();
  arena.free_items();
  free_root(&mem_root, MYF(0));
  my_free(lock);
  init();
}  
  
/* Free all allocated data */

SQL_HANDLER::~SQL_HANDLER()
{
  reset();
  my_free(base_data);
}

/*
  Get hash key and hash key length.

  SYNOPSIS
    mysql_ha_hash_get_key()
    tables                      Pointer to the hash object.
    key_len_p   (out)           Pointer to the result for key length.
    first                       Unused.

  DESCRIPTION
    The hash object is an TABLE_LIST struct.
    The hash key is the alias name.
    The hash key length is the alias name length plus one for the
    terminating NULL character.

  RETURN
    Pointer to the TABLE_LIST struct.
*/

static const uchar *mysql_ha_hash_get_key(const void *table_, size_t *key_len,
                                          my_bool)
{
  auto table= static_cast<const SQL_HANDLER *>(table_);
  *key_len= table->handler_name.length + 1 ; /* include '\0' in comparisons */
  return reinterpret_cast<const uchar *>(table->handler_name.str);
}


/*
  Free an hash object.

  SYNOPSIS
    mysql_ha_hash_free()
    tables                      Pointer to the hash object.

  DESCRIPTION
    The hash object is an TABLE_LIST struct.

  RETURN
    Nothing
*/

static void mysql_ha_hash_free(void *table)
{
  delete static_cast<SQL_HANDLER *>(table);
}

static void mysql_ha_close_childs(THD *thd, TABLE_LIST *current_table_list,
                                  TABLE_LIST **next_global)
{
  TABLE_LIST *table_list;
  DBUG_ENTER("mysql_ha_close_childs");
  DBUG_PRINT("info",("current_table_list: %p", current_table_list));
  DBUG_PRINT("info",("next_global: %p", *next_global));
  for (table_list = *next_global; table_list; table_list = *next_global)
  {
    *next_global = table_list->next_global;
    DBUG_PRINT("info",("table_name: %s.%s", table_list->table->s->db.str,
                       table_list->table->s->table_name.str));
    DBUG_PRINT("info",("parent_l: %p", table_list->parent_l));
    if (table_list->parent_l == current_table_list)
    {
      DBUG_PRINT("info",("found child"));
      TABLE *table = table_list->table;
      if (table)
      {
        table->open_by_handler= 0;
        if (!table->s->tmp_table)
        {
          (void) close_thread_table(thd, &table);
          thd->mdl_context.release_lock(table_list->mdl_request.ticket);
        }
        else
        {
          thd->mark_tmp_table_as_free_for_reuse(table);
        }
      }
      mysql_ha_close_childs(thd, table_list, next_global);
    }
    else
    {
      /* the end of child tables */
      *next_global = table_list;
      break;
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Close a HANDLER table.

  @param thd Thread identifier.
  @param tables A list of tables with the first entry to close.

  @note Though this function takes a list of tables, only the first list entry
  will be closed.
  @note handler_object is not deleted!
  @note Broadcasts refresh if it closed a table with old version.
*/

static void mysql_ha_close_table(SQL_HANDLER *handler)
{
  DBUG_ENTER("mysql_ha_close_table");
  THD *thd= handler->thd;
  TABLE *table= handler->table;
  TABLE_LIST *current_table_list= NULL, *next_global;

  /* check if table was already closed */
  if (!table)
    DBUG_VOID_RETURN;

  if ((next_global= table->file->get_next_global_for_child()))
    current_table_list= next_global->parent_l;

  table->open_by_handler= 0;
  if (!table->s->tmp_table)
  {
    /* Non temporary table. */
    if (handler->lock)
    {
      // Mark it unlocked, like in reset_lock_data()
      reset_lock_data(handler->lock, 1);
    }

    table->file->ha_index_or_rnd_end();
    close_thread_table(thd, &table);
    if (current_table_list)
      mysql_ha_close_childs(thd, current_table_list, &next_global);
    thd->mdl_context.release_lock(handler->mdl_request.ticket);
  }
  else
  {
    /* Must be a temporary table */
    table->file->ha_index_or_rnd_end();
    if (current_table_list)
      mysql_ha_close_childs(thd, current_table_list, &next_global);
    thd->mark_tmp_table_as_free_for_reuse(table);
  }
  my_free(handler->lock);
  handler->init();
  DBUG_VOID_RETURN;
}

/*
  Open a HANDLER table.

  SYNOPSIS
    mysql_ha_open()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to open.
    reopen                      Re-open a previously opened handler table.

  DESCRIPTION
    Though this function takes a list of tables, only the first list entry
    will be opened.
    'reopen' is set when a handler table is to be re-opened. In this case,
    'tables' is the pointer to the hashed SQL_HANDLER object which has been
    saved on the original open.
    'reopen' is also used to suppress the sending of an 'ok' message.

  RETURN
    FALSE OK
    TRUE  Error
*/

bool mysql_ha_open(THD *thd, TABLE_LIST *tables, SQL_HANDLER *reopen)
{
  SQL_HANDLER   *sql_handler= 0;
  uint          counter;
  bool          error;
  TABLE         *table, *backup_open_tables;
  MDL_savepoint mdl_savepoint;
  Query_arena backup_arena;
  DBUG_ENTER("mysql_ha_open");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'  reopen: %d",
                      tables->db.str, tables->table_name.str, tables->alias.str,
                      reopen != 0));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (tables->schema_table)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "HANDLER OPEN",
             INFORMATION_SCHEMA_NAME.str);
    DBUG_PRINT("exit",("ERROR"));
    DBUG_RETURN(TRUE);
  }

  if (! my_hash_inited(&thd->handler_tables_hash))
  {
    /*
      HASH entries are of type SQL_HANDLER
    */
    if (my_hash_init(key_memory_THD_handler_tables_hash,
                     &thd->handler_tables_hash, &my_charset_latin1,
                     HANDLER_TABLES_HASH_SIZE, 0, 0, mysql_ha_hash_get_key,
                     mysql_ha_hash_free, 0))
    {
      DBUG_PRINT("exit",("ERROR"));
      DBUG_RETURN(TRUE);
    }
  }
  else if (! reopen) /* Otherwise we have 'tables' already. */
  {
    if (my_hash_search(&thd->handler_tables_hash, (uchar*) tables->alias.str,
                       tables->alias.length + 1))
    {
      DBUG_PRINT("info",("duplicate '%s'", tables->alias.str));
      DBUG_PRINT("exit",("ERROR"));
      my_error(ER_NONUNIQ_TABLE, MYF(0), tables->alias.str);
      DBUG_RETURN(TRUE);
    }
  }

  /*
    Save and reset the open_tables list so that open_tables() won't
    be able to access (or know about) the previous list. And on return
    from open_tables(), thd->open_tables will contain only the opened
    table.

    See open_table() back-off comments for more details.
  */
  backup_open_tables= thd->open_tables;
  thd->set_open_tables(NULL);

  /*
    open_tables() will set 'tables->table' if successful.
    It must be NULL for a real open when calling open_tables().
  */
  DBUG_ASSERT(! tables->table);

  /*
    We can't request lock with explicit duration for this table
    right from the start as open_tables() can't handle properly
    back-off for such locks.
  */
  MDL_REQUEST_INIT(&tables->mdl_request, MDL_key::TABLE, tables->db.str,
                   tables->table_name.str, MDL_SHARED_READ, MDL_TRANSACTION);
  mdl_savepoint= thd->mdl_context.mdl_savepoint();

  /* for now HANDLER can be used only for real TABLES */
  tables->required_type= TABLE_TYPE_NORMAL;

  /*
    We use open_tables() here, rather than, say,
    open_ltable() or open_table() because we would like to be able
    to open a temporary table.
  */
  error= (thd->open_temporary_tables(tables) ||
          open_tables(thd, &tables, &counter, 0));

  if (unlikely(error))
    goto err;

  table= tables->table;

  /* There can be only one table in '*tables'. */
  if (! (table->file->ha_table_flags() & HA_CAN_SQL_HANDLER))
  {
    my_error(ER_ILLEGAL_HA, MYF(0), table->file->table_type(),
             table->s->db.str, table->s->table_name.str);
    goto err;
  }

  DBUG_PRINT("info",("clone_tickets start"));
  for (TABLE_LIST *table_list= tables; table_list;
    table_list= table_list->next_global)
  {
    DBUG_PRINT("info",("table_list %s.%s", table_list->table->s->db.str,
      table_list->table->s->table_name.str));
  if (table_list->mdl_request.ticket &&
      thd->mdl_context.has_lock(mdl_savepoint, table_list->mdl_request.ticket))
  {
      DBUG_PRINT("info",("clone_tickets"));
    /* The ticket returned is within a savepoint. Make a copy.  */
    error= thd->mdl_context.clone_ticket(&table_list->mdl_request);
    table_list->table->mdl_ticket= table_list->mdl_request.ticket;
    if (unlikely(error))
      goto err;
  }
  }
  DBUG_PRINT("info",("clone_tickets end"));

  if (! reopen)
  {
    /* copy data to sql_handler */
    if (!(sql_handler= new SQL_HANDLER(thd)))
      goto err;
    init_alloc_root(PSI_INSTRUMENT_ME, &sql_handler->mem_root, 1024, 0,
                    MYF(MY_THREAD_SPECIFIC));

    sql_handler->db.length= tables->db.length;
    sql_handler->table_name.length= tables->table_name.length;
    sql_handler->handler_name.length= tables->alias.length;

    if (!(my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                          &sql_handler->base_data,
                          (uint) sql_handler->db.length + 1,
                          &sql_handler->table_name.str,
                          (uint) sql_handler->table_name.length + 1,
                          &sql_handler->handler_name.str,
                          (uint) sql_handler->handler_name.length + 1,
                          NullS)))
      goto err;
    sql_handler->db.str= sql_handler->base_data;
    memcpy((char*) sql_handler->db.str, tables->db.str, tables->db.length +1);
    memcpy((char*) sql_handler->table_name.str, tables->table_name.str,
           tables->table_name.length+1);
    memcpy((char*) sql_handler->handler_name.str, tables->alias.str,
           tables->alias.length +1);

    /* add to hash */
    if (my_hash_insert(&thd->handler_tables_hash, (uchar*) sql_handler))
      goto err;
  }
  else
  {
    sql_handler= reopen;
    sql_handler->reset();
  }    
  sql_handler->table= table;

  if (!(sql_handler->lock= get_lock_data(thd, &sql_handler->table, 1,
                                         GET_LOCK_STORE_LOCKS)))
    goto err;

  /* Get a list of all fields for send_fields */
  thd->set_n_backup_active_arena(&sql_handler->arena, &backup_arena);
  error= table->fill_item_list(&sql_handler->fields);
  thd->restore_active_arena(&sql_handler->arena, &backup_arena);
  if (unlikely(error))
    goto err;

  sql_handler->mdl_request.move_from(tables->mdl_request);

  /* Always read all columns */
  table->read_set= &table->s->all_set;

  /* Restore the state. */
  thd->set_open_tables(backup_open_tables);
  DBUG_PRINT("info",("set_lock_duration start"));
  if (sql_handler->mdl_request.ticket)
  {
    thd->mdl_context.set_lock_duration(sql_handler->mdl_request.ticket,
                                       MDL_EXPLICIT);
    thd->mdl_context.set_needs_thr_lock_abort(TRUE);
  }
  for (TABLE_LIST *table_list= tables->next_global; table_list;
    table_list= table_list->next_global)
  {
    DBUG_PRINT("info",("table_list %s.%s", table_list->table->s->db.str,
      table_list->table->s->table_name.str));
    if (table_list->mdl_request.ticket)
    {
      thd->mdl_context.set_lock_duration(table_list->mdl_request.ticket,
                                         MDL_EXPLICIT);
      thd->mdl_context.set_needs_thr_lock_abort(TRUE);
    }
  }
  DBUG_PRINT("info",("set_lock_duration end"));

  /*
    If it's a temp table, don't reset table->query_id as the table is
    being used by this handler. For non-temp tables we use this flag
    in asserts.
  */
  for (TABLE_LIST *table_list= tables; table_list;
    table_list= table_list->next_global)
  {
    table_list->table->open_by_handler= 1;
  }

  if (! reopen)
    my_ok(thd);
  DBUG_PRINT("exit",("OK"));
  DBUG_RETURN(FALSE);

err:
  /*
    No need to rollback statement transaction, it's not started.
    If called with reopen flag, no need to rollback either,
    it will be done at statement end.
  */
  DBUG_ASSERT(thd->transaction->stmt.is_empty());
  close_thread_tables(thd);
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);
  thd->set_open_tables(backup_open_tables);
  if (sql_handler)
  {
    if (!reopen)
      my_hash_delete(&thd->handler_tables_hash, (uchar*) sql_handler);
    else
      sql_handler->reset(); // or should it be init() ?
  }
  DBUG_PRINT("exit",("ERROR"));
  DBUG_RETURN(TRUE);
}


/*
  Close a HANDLER table by alias or table name

  SYNOPSIS
    mysql_ha_close()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to close.

  DESCRIPTION
    Closes the table that is associated (on the handler tables hash) with the
    name (table->alias) of the specified table.

  RETURN
    FALSE ok
    TRUE  error
*/

bool mysql_ha_close(THD *thd, TABLE_LIST *tables)
{
  SQL_HANDLER *handler;
  DBUG_ENTER("mysql_ha_close");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'",
                      tables->db.str, tables->table_name.str, tables->alias.str));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if ((my_hash_inited(&thd->handler_tables_hash)) &&
      (handler= (SQL_HANDLER*) my_hash_search(&thd->handler_tables_hash,
                                              (const uchar*) tables->alias.str,
                                              tables->alias.length + 1)))
  {
    mysql_ha_close_table(handler);
    my_hash_delete(&thd->handler_tables_hash, (uchar*) handler);
  }
  else
  {
    my_error(ER_UNKNOWN_TABLE, MYF(0), tables->alias.str, "HANDLER");
    DBUG_PRINT("exit",("ERROR"));
    DBUG_RETURN(TRUE);
  }

  /*
    Mark MDL_context as no longer breaking protocol if we have
    closed last HANDLER.
  */
  if (! thd->handler_tables_hash.records)
    thd->mdl_context.set_needs_thr_lock_abort(FALSE);

  my_ok(thd);
  DBUG_PRINT("exit", ("OK"));
  DBUG_RETURN(FALSE);
}


/**
   Finds an open HANDLER table.

   @params name		Name of handler to open

   @return 0 failure
   @return handler
*/  

static SQL_HANDLER *mysql_ha_find_handler(THD *thd, const LEX_CSTRING *name)
{
  SQL_HANDLER *handler;
  if ((my_hash_inited(&thd->handler_tables_hash)) &&
      (handler= (SQL_HANDLER*) my_hash_search(&thd->handler_tables_hash,
                                              (const uchar*) name->str,
                                              name->length + 1)))
  {
    DBUG_PRINT("info-in-hash",("'%s'.'%s' as '%s' table: %p",
                               handler->db.str,
                               handler->table_name.str,
                               handler->handler_name.str, handler->table));
    if (!handler->table)
    {
      /* The handler table has been closed. Re-open it. */
      TABLE_LIST tmp;
      tmp.init_one_table(&handler->db, &handler->table_name,
                         &handler->handler_name, TL_READ);

      if (mysql_ha_open(thd, &tmp, handler))
      {
        DBUG_PRINT("exit",("reopen failed"));
        return 0;
      }
    }
  }
  else
  {
    my_error(ER_UNKNOWN_TABLE, MYF(0), name->str, "HANDLER");
    return 0;
  }
  return handler;
}


/**
   Check that condition and key name are ok

   @param handler
   @param mode		Read mode (RFIRST, RNEXT etc...)
   @param keyname	Key to use.
   @param key_expr      List of key column values
   @param cond		Where clause
   @param in_prepare	If we are in prepare phase (we can't evaluate items yet)

   @return 0 ok
   @return 1 error

   In ok, then values of used key and mode is stored in sql_handler
*/

static bool
mysql_ha_fix_cond_and_key(SQL_HANDLER *handler, 
                          enum enum_ha_read_modes mode, const char *keyname,
                          List<Item> *key_expr,
                          enum ha_rkey_function ha_rkey_mode,
                          Item *cond, bool in_prepare)
{
  THD *thd= handler->thd;
  TABLE *table= handler->table;
  if (cond)
  {
    bool ret;
    Item::vcol_func_processor_result res;

    /* This can only be true for temp tables */
    if (table->query_id != thd->query_id)
      cond->cleanup();                          // File was reopened

    ret= cond->walk(&Item::check_handler_func_processor, 0, &res);
    if (ret || res.errors)
    {
      my_error(ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED, MYF(0), res.name,
               "WHERE", "HANDLER");
      return 1;                                 // ROWNUM() used
    }
    thd->where= THD_WHERE::WHERE_CLAUSE;
    if (cond->fix_fields_if_needed_for_bool(thd, &cond))
      return 1;
  }

  if (keyname)
  {
    /* Check if same as last keyname. If not, do a full lookup */
    if (handler->keyno < 0 ||
        !Lex_ident_column(Lex_cstring_strlen(keyname)).
          streq(table->s->key_info[handler->keyno].name))
    {
      if ((handler->keyno= find_type(keyname, &table->s->keynames,
                                     FIND_TYPE_NO_PREFIX) - 1) < 0)
      {
        my_error(ER_KEY_DOES_NOT_EXISTS, MYF(0), keyname,
                 handler->handler_name.str);
        return 1;
      }
    }

    const KEY *c_key= table->s->key_info + handler->keyno;

    if (c_key->algorithm == HA_KEY_ALG_FULLTEXT ||
        c_key->algorithm == HA_KEY_ALG_VECTOR ||
        (ha_rkey_mode != HA_READ_KEY_EXACT &&
         (table->key_info[handler->keyno].index_flags &
          (HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE)) == 0))
    {
      my_error(ER_KEY_DOESNT_SUPPORT, MYF(0),
               table->file->index_type(handler->keyno), c_key->name.str);
      return 1;
    }

    /* Check key parts */
    if (mode == RKEY)
    {
      KEY *keyinfo= table->key_info + handler->keyno;
      KEY_PART_INFO *key_part= keyinfo->key_part;
      List_iterator<Item> it_ke(*key_expr);
      Item *item;
      key_part_map keypart_map;
      uint key_len;

      if (key_expr->elements > keyinfo->user_defined_key_parts)
      {
        my_error(ER_TOO_MANY_KEY_PARTS, MYF(0),
                 keyinfo->user_defined_key_parts);
        return 1;
      }

      if (key_expr->elements < keyinfo->user_defined_key_parts &&
          (table->key_info[handler->keyno].index_flags & HA_ONLY_WHOLE_INDEX))
      {
        my_error(ER_KEY_DOESNT_SUPPORT, MYF(0),
                 table->file->index_type(handler->keyno), keyinfo->name.str);
        return 1;
      }

      thd->where= THD_WHERE::HANDLER_STATEMENT;
      for (keypart_map= key_len=0 ; (item=it_ke++) ; key_part++)
      {
	/* note that 'item' can be changed by fix_fields() call */
        if (item->fix_fields_if_needed_for_scalar(thd, it_ke.ref()))
          return 1;
        item= *it_ke.ref();
	if (item->used_tables() & ~(RAND_TABLE_BIT | PARAM_TABLE_BIT))
        {
          my_error(ER_WRONG_ARGUMENTS,MYF(0),"HANDLER ... READ");
	  return 1;
        }
        if (!in_prepare)
        {
          MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->write_set);
          int res= item->save_in_field(key_part->field, 1);
          dbug_tmp_restore_column_map(&table->write_set, old_map);
          if (res < 0 || thd->is_error())
            return 1;
        }
        key_len+= key_part->store_length;
        keypart_map= (keypart_map << 1) | 1;
      }
      handler->keypart_map= keypart_map;
      handler->key_len= key_len;
    }
    else
    {
      /*
        Check if the same index involved.
        We need to always do this check because we may not have yet
        called the handler since the last keyno change.
      */
      if ((uint) handler->keyno != table->file->get_index())
      {
        if (mode == RNEXT)
          mode= RFIRST;
        else if (mode == RPREV)
          mode= RLAST;
      }
    }
  }
  else if (table->file->inited != handler::RND)
  {
    /* Convert RNEXT to RFIRST if we haven't started row scan */
    if (mode == RNEXT)
      mode= RFIRST;
  }
  handler->mode= mode;                          // Store adjusted mode
  return 0;
}

/*
  Read from a HANDLER table.

  SYNOPSIS
    mysql_ha_read()
    thd                         Thread identifier.
    tables                      A list of tables with the first entry to read.
    mode
    keyname
    key_expr
    ha_rkey_mode
    cond
    select_limit_cnt
    offset_limit_cnt

  RETURN
    FALSE ok
    TRUE  error
*/
 
bool mysql_ha_read(THD *thd, TABLE_LIST *tables,
                   enum enum_ha_read_modes mode, const char *keyname,
                   List<Item> *key_expr,
                   enum ha_rkey_function ha_rkey_mode, Item *cond,
                   ha_rows select_limit_cnt, ha_rows offset_limit_cnt)
{
  SQL_HANDLER   *handler;
  TABLE         *table;
  Protocol	*protocol= thd->protocol;
  char		buff[MAX_FIELD_WIDTH];
  String	buffer(buff, sizeof(buff), system_charset_info);
  int           error, keyno;
  uint          num_rows;
  uchar		*UNINIT_VAR(key);
  MDL_deadlock_and_lock_abort_error_handler sql_handler_lock_error;
  DBUG_ENTER("mysql_ha_read");
  DBUG_PRINT("enter",("'%s'.'%s' as '%s'",
                      tables->db.str, tables->table_name.str, tables->alias.str));

  if (thd->locked_tables_mode)
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(TRUE);
  }

retry:
  if (!(handler= mysql_ha_find_handler(thd, &tables->alias)))
    goto err0;

  if (thd->transaction->xid_state.check_has_uncommitted_xa())
    goto err0;

  table= handler->table;
  tables->table= table;                         // This is used by fix_fields
  table->pos_in_table_list= tables;

  if (handler->lock->table_count > 0)
  {
    int lock_error;

    THR_LOCK_DATA **pos,**end;
    for (pos= handler->lock->locks,
           end= handler->lock->locks + handler->lock->lock_count;
         pos < end;
         pos++)
    {
      pos[0]->type= pos[0]->org_type;
    }

    /* save open_tables state */
    TABLE* backup_open_tables= thd->open_tables;
    /* Always a one-element list, see mysql_ha_open(). */
    DBUG_ASSERT(table->next == NULL || table->s->tmp_table);
    /*
      mysql_lock_tables() needs thd->open_tables to be set correctly to
      be able to handle aborts properly.
    */
    thd->set_open_tables(table);

    sql_handler_lock_error.init();
    thd->push_internal_handler(&sql_handler_lock_error);

    lock_error= mysql_lock_tables(thd, handler->lock,
                                  (table->s->tmp_table == NO_TMP_TABLE ?
                                    MYSQL_LOCK_NOT_TEMPORARY : 0));

    thd->pop_internal_handler();

    /*
      In 5.1 and earlier, mysql_lock_tables() could replace the TABLE
      object with another one (reopen it). This is no longer the case
      with new MDL.
    */
    DBUG_ASSERT(table == thd->open_tables);
    /* Restore previous context. */
    thd->set_open_tables(backup_open_tables);

    if (sql_handler_lock_error.need_reopen())
    {
      DBUG_ASSERT(lock_error && !thd->is_error());
      /*
        Always close statement transaction explicitly,
        so that the engine doesn't have to count locks.
        There should be no need to perform transaction
        rollback due to deadlock.
      */
      DBUG_ASSERT(! thd->transaction_rollback_request);
      trans_rollback_stmt(thd);
      mysql_ha_close_table(handler);
      if (thd->stmt_arena->is_stmt_execute())
      {
        /*
          As we have already sent field list and types to the client, we can't
          handle any changes in the table format for prepared statements.
          Better to force a reprepare.
        */
        my_error(ER_NEED_REPREPARE, MYF(0));
        goto err0;
      }
      goto retry;
    }

    if (unlikely(lock_error))
      goto err0; // mysql_lock_tables() printed error message already
  }

  if (mysql_ha_fix_cond_and_key(handler, mode, keyname, key_expr,
                                ha_rkey_mode, cond, 0))
    goto err;
  mode= handler->mode;
  keyno= handler->keyno;

  protocol->send_result_set_metadata(&handler->fields,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);

  /*
    In ::external_lock InnoDB resets the fields which tell it that
    the handle is used in the HANDLER interface. Tell it again that
    we are using it for HANDLER.
  */

  table->file->init_table_handle_for_HANDLER();

  for (num_rows=0; num_rows < select_limit_cnt; )
  {
    switch (mode) {
    case RNEXT:
      if (table->file->inited != handler::NONE)
      {
        if ((error= table->file->can_continue_handler_scan()))
          break;
        if (keyname)
        {
          /* Check if we read from the same index. */
          DBUG_ASSERT((uint) keyno == table->file->get_index());
          error= table->file->ha_index_next(table->record[0]);
        }
        else
          error= table->file->ha_rnd_next(table->record[0]);
        break;
      }
      /* else fall through */
    case RFIRST:
      if (keyname)
      {
        if (likely(!(error= table->file->ha_index_or_rnd_end())) &&
            likely(!(error= table->file->ha_index_init(keyno, 1))))
          error= table->file->ha_index_first(table->record[0]);
      }
      else
      {
        if (likely(!(error= table->file->ha_index_or_rnd_end())) &&
	    likely(!(error= table->file->ha_rnd_init(1))))
          error= table->file->ha_rnd_next(table->record[0]);
      }
      mode= RNEXT;
      break;
    case RPREV:
      DBUG_ASSERT(keyname != 0);
      /* Check if we read from the same index. */
      DBUG_ASSERT((uint) keyno == table->file->get_index());
      if (table->file->inited != handler::NONE)
      {
        if ((error= table->file->can_continue_handler_scan()))
          break;
        error= table->file->ha_index_prev(table->record[0]);
        break;
      }
      /* else fall through */
    case RLAST:
      DBUG_ASSERT(keyname != 0);
      if (likely(!(error= table->file->ha_index_or_rnd_end())) &&
          likely(!(error= table->file->ha_index_init(keyno, 1))))
        error= table->file->ha_index_last(table->record[0]);
      mode=RPREV;
      break;
    case RNEXT_SAME:
      /* Continue scan on "(keypart1,keypart2,...)=(c1, c2, ...)  */
      DBUG_ASSERT(keyname != 0);
      error= table->file->ha_index_next_same(table->record[0], key,
                                             handler->key_len);
      break;
    case RKEY:
    {
      DBUG_ASSERT(keyname != 0);

      if (unlikely(!(key= thd->calloc<uchar>(ALIGN_SIZE(handler->key_len)))))
	goto err;
      if (unlikely((error= table->file->ha_index_or_rnd_end())))
        break;
      key_copy(key, table->record[0], table->key_info + keyno,
               handler->key_len);
      if (unlikely(!(error= table->file->ha_index_init(keyno, 1))))
        error= table->file->ha_index_read_map(table->record[0],
                                              key, handler->keypart_map,
                                              ha_rkey_mode);
      mode= rkey_to_rnext[(int)ha_rkey_mode];
      break;
    }
    default:
      my_error(ER_ILLEGAL_HA, MYF(0), table->file->table_type(),
               table->s->db.str, table->s->table_name.str);
      goto err;
    }

    if (unlikely(error))
    {
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      {
        /* Don't give error in the log file for some expected problems */
        if (error != HA_ERR_RECORD_CHANGED && error != HA_ERR_WRONG_COMMAND)
          sql_print_error("mysql_ha_read: Got error %d when reading "
                          "table '%s'",
                          error, tables->table_name.str);
        table->file->print_error(error,MYF(0));
        table->file->ha_index_or_rnd_end();
        goto err;
      }
      goto ok;
    }
    thd->inc_examined_row_count();
    if (cond && !cond->val_bool())
    {
      if (thd->is_error())
        goto err;
      continue;
    }
    if (num_rows >= offset_limit_cnt)
    {
      protocol->prepare_for_resend();

      if (protocol->send_result_set_row(&handler->fields))
        goto err;

      protocol->write();
      thd->inc_sent_row_count(1);
    }
    num_rows++;
  }
ok:
  /*
    Always close statement transaction explicitly,
    so that the engine doesn't have to count locks.
  */
  trans_commit_stmt(thd);
  mysql_unlock_tables(thd, handler->lock, 0);
  my_eof(thd);
  DBUG_PRINT("exit",("OK"));
  DBUG_RETURN(FALSE);

err:
  trans_rollback_stmt(thd);
  mysql_unlock_tables(thd, handler->lock, 0);
err0:
  DBUG_PRINT("exit",("ERROR"));
  DBUG_RETURN(TRUE);
}


/**
   Prepare for handler read

   For parameters, see mysql_ha_read()
*/

SQL_HANDLER *mysql_ha_read_prepare(THD *thd, TABLE_LIST *tables,
                                   enum enum_ha_read_modes mode,
                                   const char *keyname,
                                   List<Item> *key_expr,
                                   enum ha_rkey_function ha_rkey_mode,
                                   Item *cond)
{
  SQL_HANDLER *handler;
  DBUG_ENTER("mysql_ha_read_prepare");
  if (!(handler= mysql_ha_find_handler(thd, &tables->alias)))
    DBUG_RETURN(0);
  tables->table= handler->table;         // This is used by fix_fields
  handler->table->pos_in_table_list= tables;
  if (mysql_ha_fix_cond_and_key(handler, mode, keyname, key_expr,
                                ha_rkey_mode, cond, 1))
    DBUG_RETURN(0);
  DBUG_RETURN(handler);
}

  

/**
  Scan the handler tables hash for matching tables.

  @param thd Thread identifier.
  @param tables The list of tables to remove.

  @return Pointer to head of linked list (TABLE_LIST::next_local) of matching
          TABLE_LIST elements from handler_tables_hash. Otherwise, NULL if no
          table was matched.
*/

static SQL_HANDLER *mysql_ha_find_match(THD *thd, TABLE_LIST *tables)
{
  SQL_HANDLER *hash_tables, *head= NULL;
  TABLE_LIST *first= tables;
  DBUG_ENTER("mysql_ha_find_match");

  /* search for all handlers with matching table names */
  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (SQL_HANDLER*) my_hash_element(&thd->handler_tables_hash, i);

    for (tables= first; tables; tables= tables->next_local)
    {
      if (tables->is_anonymous_derived_table())
        continue;
      if ((! tables->db.str[0] ||
          tables->get_db_name().streq(hash_tables->db)) &&
          tables->get_table_name().streq(hash_tables->table_name))
      {
        /* Link into hash_tables list */
        hash_tables->next= head;
        head= hash_tables;
        break;
      }
    }
  }
  DBUG_RETURN(head);
}


/**
  Remove matching tables from the HANDLER's hash table.

  @param thd Thread identifier.
  @param tables The list of tables to remove.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_rm_tables(THD *thd, TABLE_LIST *tables)
{
  SQL_HANDLER *hash_tables, *next;
  DBUG_ENTER("mysql_ha_rm_tables");

  DBUG_ASSERT(tables);

  hash_tables= mysql_ha_find_match(thd, tables);

  while (hash_tables)
  {
    next= hash_tables->next;
    if (hash_tables->table)
      mysql_ha_close_table(hash_tables);
    my_hash_delete(&thd->handler_tables_hash, (uchar*) hash_tables);
    hash_tables= next;
  }

  /*
    Mark MDL_context as no longer breaking protocol if we have
    closed last HANDLER.
  */
  if (! thd->handler_tables_hash.records)
    thd->mdl_context.set_needs_thr_lock_abort(FALSE);

  DBUG_VOID_RETURN;
}


/**
  Close cursors of matching tables from the HANDLER's hash table.

  @param thd Thread identifier.
  @param tables The list of tables to flush.
*/

void mysql_ha_flush_tables(THD *thd, TABLE_LIST *all_tables)
{
  DBUG_ENTER("mysql_ha_flush_tables");

  for (TABLE_LIST *table_list= all_tables; table_list;
       table_list= table_list->next_global)
  {
    SQL_HANDLER *hash_tables= mysql_ha_find_match(thd, table_list);
    /* Close all aliases of the same table. */
    while (hash_tables)
    {
      SQL_HANDLER *next_local= hash_tables->next;
      if (hash_tables->table)
        mysql_ha_close_table(hash_tables);
      hash_tables= next_local;
    }
  }

  DBUG_VOID_RETURN;
}


/**
  Flush (close and mark for re-open) all tables that should be should
  be reopen.

  @param thd Thread identifier.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_flush(THD *thd)
{
  SQL_HANDLER *hash_tables;
  DBUG_ENTER("mysql_ha_flush");

  /*
    Don't try to flush open HANDLERs when we're working with
    system tables. The main MDL context is backed up and we can't
    properly release HANDLER locks stored there.
  */
  if (thd->state_flags & Open_tables_state::BACKUPS_AVAIL)
    DBUG_VOID_RETURN;

  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (SQL_HANDLER*) my_hash_element(&thd->handler_tables_hash, i);
    /*
      TABLE::mdl_ticket is 0 for temporary tables so we need extra check.
    */
    if (hash_tables->table &&
        ((hash_tables->table->mdl_ticket &&
         hash_tables->table->mdl_ticket->has_pending_conflicting_lock()) ||
         (!hash_tables->table->s->tmp_table &&
          hash_tables->table->s->tdc->flushed)))
      mysql_ha_close_table(hash_tables);
  }

  DBUG_VOID_RETURN;
}


/**
  Close all HANDLER's tables.

  @param thd Thread identifier.

  @note Broadcasts refresh if it closed a table with old version.
*/

void mysql_ha_cleanup_no_free(THD *thd)
{
  SQL_HANDLER *hash_tables;
  DBUG_ENTER("mysql_ha_cleanup_no_free");

  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (SQL_HANDLER*) my_hash_element(&thd->handler_tables_hash, i);
    if (hash_tables->table)
      mysql_ha_close_table(hash_tables);
  }
  DBUG_VOID_RETURN;
}


void mysql_ha_cleanup(THD *thd)
{
  DBUG_ENTER("mysql_ha_cleanup");
  mysql_ha_cleanup_no_free(thd);
  my_hash_free(&thd->handler_tables_hash);
  DBUG_VOID_RETURN;
}


/**
  Set explicit duration for metadata locks corresponding to open HANDLERs
  to protect them from being released at the end of transaction.

  @param thd Thread identifier.
*/

void mysql_ha_set_explicit_lock_duration(THD *thd)
{
  SQL_HANDLER *hash_tables;
  DBUG_ENTER("mysql_ha_set_explicit_lock_duration");

  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    hash_tables= (SQL_HANDLER*) my_hash_element(&thd->handler_tables_hash, i);
    if (hash_tables->table && hash_tables->table->mdl_ticket)
      thd->mdl_context.set_lock_duration(hash_tables->table->mdl_ticket,
                                         MDL_EXPLICIT);
  }
  DBUG_VOID_RETURN;
}


/**
  Remove temporary tables from the HANDLER's hash table. The reason
  for having a separate function, rather than calling
  mysql_ha_rm_tables() is that it is not always feasible (e.g. in
  THD::close_temporary_tables) to obtain a TABLE_LIST containing the
  temporary tables.

  @See THD::close_temporary_tables()
  @param thd Thread identifier.
*/
void mysql_ha_rm_temporary_tables(THD *thd)
{
  DBUG_ENTER("mysql_ha_rm_temporary_tables");

  TABLE_LIST *tmp_handler_tables= NULL;
  for (uint i= 0; i < thd->handler_tables_hash.records; i++)
  {
    TABLE_LIST *handler_table= reinterpret_cast<TABLE_LIST*>
      (my_hash_element(&thd->handler_tables_hash, i));

    if (handler_table->table && handler_table->table->s->tmp_table)
    {
      handler_table->next_local= tmp_handler_tables;
      tmp_handler_tables= handler_table;
    }
  }

  if (tmp_handler_tables)
    mysql_ha_rm_tables(thd, tmp_handler_tables);

  DBUG_VOID_RETURN;
}

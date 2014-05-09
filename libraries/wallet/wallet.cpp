#include <bts/wallet/wallet.hpp>
#include <bts/blockchain/config.hpp>
#include <bts/db/level_map.hpp>
#include <fc/crypto/aes.hpp>

#include <iostream>

#define EXTRA_PRIVATE_KEY_BASE (100*1000*1000ll)
#define ACCOUNT_INDEX_BASE     (200*1000*1000ll)


namespace bts { namespace wallet {

   namespace detail 
   {
      class wallet_impl
      {
         public:
            wallet_impl():_last_contact_index(-1)
            {
               // this effectively sets the maximum transaction size to 10KB without
               // custom code.  We will set this initial fee high while the network
               // is of lower value to prevent initial spam attacks.   We also
               // want to subsidize the delegates earlly on.
               _current_fee = asset( 1000*100, 0 );
            }

            wallet* self;

            asset _current_fee;

            /** meta_record_property_enum is the key */
            std::unordered_map<int,wallet_meta_record>                          _meta;

            chain_database_ptr                                                  _blockchain;

            /** map record id to encrypted record data */
            bts::db::level_map< uint32_t, wallet_record >                       _wallet_db;
                                                                                
            /** the password required to decrypt the wallet records */          
            fc::sha512                                                          _wallet_password;
                                                                                
            fc::optional<master_key_record>                                     _master_key;
                                                                                
            /** lookup contact state */                                         
            std::unordered_map<uint32_t,wallet_contact_record>                  _contacts;
            int32_t                                                             _last_contact_index;
                                                                                
            /** registered accounts */                                             
            std::unordered_map<account_id_type,wallet_account_record>           _accounts;
                                                                                
            /** registered names */                                             
            std::unordered_map<name_id_type,wallet_name_record>                 _names;
                                                                                
            /** registered assets */                                            
            std::unordered_map<asset_id_type,wallet_asset_record>               _assets;

            /** all transactions to or from this wallet */
            std::unordered_map<transaction_id_type,wallet_transaction_record>   _transactions;

            /** caches all addresses and where in the hierarchial tree they can be found */
            std::unordered_map<address, hkey_index>                             _receive_keys;

            /** used when hkey_index == (X,Y,-N) to lookup foreign private keys, where
             * N is the key into _extra_receive_keys
             **/
            std::unordered_map<int32_t, private_key_record >                   _extra_receive_keys;

            std::unordered_map<std::string, uint32_t>                          _contact_name_index;


            int32_t get_new_index()
            {
               auto meta_itr = _meta.find( next_record_number );
               if( meta_itr == _meta.end() )
               {
                  _meta[next_record_number] = wallet_meta_record( 0, next_record_number, 1 );
                  _wallet_db.store( 0, _meta[next_record_number] );
                  return 1;
               }
               auto next_index = meta_itr->second.value.as_int64() + 1;
               meta_itr->second.value = next_index;
               _wallet_db.store( meta_itr->second.index, meta_itr->second );
               return next_index;
            }

            /** contact indexes are tracked independently from record indexes because
             * the goal is to focus them early in the hierarchial wallet number 
             * sequence to make recovery more feasible.
             */
            int32_t get_next_contact_index()
            {
                auto meta_itr = _meta.find( last_contact_index );
                if( meta_itr == _meta.end() )
                {
                   auto new_index = get_new_index();
                   _meta[last_contact_index] = wallet_meta_record( new_index, last_contact_index, 1 );
                   _wallet_db.store( new_index, _meta[last_contact_index] );
                   return 1;
                }
                auto next_index = meta_itr->second.value.as_int64() + 1;
                meta_itr->second.value = next_index;
                _wallet_db.store( meta_itr->second.index, meta_itr->second );
                return next_index;
            }

            asset  get_default_fee()
            {
                auto meta_itr = _meta.find( default_transaction_fee );
                if( meta_itr == _meta.end() )
                {
                   auto new_index = get_new_index();
                   _meta[default_transaction_fee] = wallet_meta_record( new_index, 
                                                                        default_transaction_fee, 
                                                                        fc::variant(asset( 1000*100, 0)) );
                   _wallet_db.store( new_index, _meta[last_contact_index] );
                   return _meta[default_transaction_fee].value.as<asset>();
                }
                return meta_itr->second.value.as<asset>();
            }

            int32_t get_last_contact_index()
            {
                auto meta_itr = _meta.find( last_contact_index );
                if( meta_itr == _meta.end() )
                {
                   auto new_index = get_new_index();
                   _meta[last_contact_index] = wallet_meta_record( new_index, last_contact_index, 0 );
                   _wallet_db.store( new_index, _meta[last_contact_index] );
                   return 0;
                }
                return meta_itr->second.value.as_int64();
            }

            void initialize_wallet()
            {
               FC_ASSERT( _wallet_password != fc::sha512(), "No wallet password specified" );

               auto key = fc::ecc::private_key::generate();
               auto chain_code = fc::ecc::private_key::generate();

               extended_private_key exp( key.get_secret(), chain_code.get_secret() );
              
               _master_key = master_key_record();
               _master_key->index = get_new_index();
               _master_key->encrypted_key = fc::aes_encrypt( _wallet_password, fc::raw::pack(exp) );

               _wallet_db.store( _master_key->index, wallet_record( *_master_key ) );
            }

            /** the key index that the account belongs to */
            void index_account( const hkey_index& idx, const account_record& account )
            {
               auto id = account.id();
               auto itr = _accounts.find( id );
               if( itr == _accounts.end() )
               {
                  _accounts[id] = wallet_account_record( get_new_index(), account );
                  itr = _accounts.find( id );
               }
               else
               {
                  itr->second = wallet_account_record( itr->second.index, account );
               }
               _wallet_db.store( itr->second.index, itr->second );
            }

            void index_name( const hkey_index& idx, const name_record& name )
            {
                auto itr = _names.find( name.id );
                if( itr != _names.end() )
                {
                   itr->second = wallet_name_record( itr->second.index, name );
                }
                else
                {
                   _names[name.id] = wallet_name_record( get_new_index(), name );
                   itr = _names.find( name.id );
                }
                _wallet_db.store( itr->second.index, itr->second );
            }

            void index_asset( const asset_record& a )
            {
                auto itr = _assets.find( a.id );
                if( itr != _assets.end() )
                {
                   itr->second = wallet_asset_record( itr->second.index, a );
                }
                else
                {
                   _assets[a.id] = wallet_asset_record( get_new_index(), a );
                   itr = _assets.find( a.id );
                }
                _wallet_db.store( itr->second.index, itr->second );
            }


            void scan_account( const account_record& account )
            {
                switch( (withdraw_condition_types)account.condition.condition )
                {
                   case withdraw_signature_type:
                   {
                      auto owner = account.condition.as<withdraw_with_signature>().owner;
                      if( is_my_address( owner ) )
                          index_account( _receive_keys.find( owner )->second, account );
                      break;
                   }
                   case withdraw_multi_sig_type:
                   {
                      auto cond = account.condition.as<withdraw_with_multi_sig>();
                      for( auto owner : cond.owners )
                      {
                         if( is_my_address( owner ) )
                         {
                            index_account( _receive_keys.find( owner )->second, account );
                            break;
                         }
                      }
                   }
                   case withdraw_password_type:
                   {
                      auto cond = account.condition.as<withdraw_with_password>();
                      auto itr = _receive_keys.find( cond.payee );
                      if( itr != _receive_keys.end() )
                      {
                         index_account( itr->second, account );
                         break;
                      }
                      itr = _receive_keys.find( cond.payor );
                      if( itr != _receive_keys.end() )
                      {
                         index_account( itr->second, account );
                         break;
                      }
                   }
                   case withdraw_option_type:
                   {
                      auto cond = account.condition.as<withdraw_option>();
                      auto itr = _receive_keys.find( cond.optionor );
                      if( itr != _receive_keys.end() )
                      {
                         index_account( itr->second, account );
                         break;
                      }
                      itr = _receive_keys.find( cond.optionee );
                      if( itr != _receive_keys.end() )
                      {
                         index_account( itr->second, account );
                         break;
                      }
                      break;
                   }
                   case withdraw_null_type:
                      break;
                }
            }

            void scan_asset( const asset_record& current_asset )
            {
                if( _names.find( current_asset.issuer_name_id ) != _names.end() )
                {

                }
            }

            void scan_name( const name_record& name )
            {
               auto current_itr = _names.find( name.id );
               if( current_itr != _names.end() )
               {
                  // do we still own it or has it been transferred to someone else.
               }
               else
               {
                  if( self->is_my_address( address( name.owner_key ) ) || self->is_my_address( address( name.active_key) ) )
                  {
                     _names[name.id] = wallet_name_record( get_new_index(), name );
                  }
                  else
                  {
                    wlog( "not our name ${r}", ("r",name) );
                  }
               }
            }

            bool scan_withdraw( const withdraw_operation& op )
            {
               auto account_itr = _accounts.find( op.account_id );
               if( account_itr != _accounts.end() )
                  return true;
               return false;
            }
            bool is_my_address( const address& a ) { return self->is_my_address( a ); }
            bool scan_first_deposit( const first_deposit_operation& op )
            {
               switch( (withdraw_condition_types) op.condition.condition )
               {
                  case withdraw_signature_type:
                     return self->is_my_address( op.condition.as<withdraw_with_signature>().owner );
                  case withdraw_multi_sig_type:
                  {
                     for( auto owner : op.condition.as<withdraw_with_multi_sig>().owners )
                        if( self->is_my_address( owner ) ) return true;
                     break;
                  }
                  case withdraw_password_type:
                  {
                     auto cond = op.condition.as<withdraw_with_password>();
                     if( is_my_address( cond.payee ) ) return true;
                     if( is_my_address( cond.payor ) ) return true;
                     break;
                  }
                  case withdraw_option_type:
                  {
                     auto cond = op.condition.as<withdraw_option>();
                     if( is_my_address( cond.optionor ) ) return true;
                     if( is_my_address( cond.optionee ) ) return true;
                     break;
                  }
                  case withdraw_null_type:
                     break;
               }
               return false;
            }

            bool scan_deposit( const deposit_operation& op )
            {
               return _accounts.find( op.account_id ) != _accounts.end();
            }

            bool scan_reserve_name( const reserve_name_operation& op )
            {
               if( is_my_address( op.owner_key ) ) return true;
               if( is_my_address( op.active_key ) ) return true;
               return false;
            }

            bool scan_update_name( const update_name_operation& op )
            {
               return _names.find( op.name_id ) != _names.end();
            }

            bool scan_create_asset( const create_asset_operation& op )
            {
               return _names.find( op.issuer_name_id ) != _names.end();
            }
            bool scan_update_asset( const update_asset_operation& op )
            {
               return _assets.find( op.asset_id ) != _assets.end();
            }
            bool scan_issue_asset( const issue_asset_operation& op )
            {
               return _assets.find( op.asset_id ) != _assets.end();
            }

            void scan_transaction_state( const transaction_evaluation_state_ptr& state )
            {
                bool mine = false;
                for( auto op : state->trx.operations )
                {
                   switch( (operation_type_enum)op.type  )
                   {
                     case withdraw_op_type:
                        mine |= scan_withdraw( op.as<withdraw_operation>() );
                        break;
                     case first_deposit_op_type:
                        mine |= scan_first_deposit( op.as<first_deposit_operation>() );
                        break;
                     case deposit_op_type:
                        mine |= scan_deposit( op.as<deposit_operation>() );
                        break;
                     case reserve_name_op_type:
                        mine |= scan_reserve_name( op.as<reserve_name_operation>() );
                        break;
                     case update_name_op_type:
                        mine |= scan_update_name( op.as<update_name_operation>() );
                        break;
                     case create_asset_op_type:
                        mine |= scan_create_asset( op.as<create_asset_operation>() );
                        break;
                     case update_asset_op_type:
                        mine |= scan_update_asset( op.as<update_asset_operation>() );
                        break;
                     case issue_asset_op_type:
                        mine |= scan_issue_asset( op.as<issue_asset_operation>() );
                        break;
                     case null_op_type:
                        break;
                   }
                }
                if( mine )
                {
                   store_transaction( state );
                }
            }
            void store_transaction( const transaction_evaluation_state_ptr& state )
            {
               auto trx_id = state->trx.id();
               auto trx_rec_itr = _transactions.find( trx_id );
               if( trx_rec_itr != _transactions.end() )
               {
                  trx_rec_itr->second.state = *state;
               }
               else
               {
                  _transactions[trx_id] = wallet_transaction_record( get_new_index(), *state );
                  trx_rec_itr = _transactions.find( trx_id );
               }   
               _wallet_db.store( trx_rec_itr->second.index, trx_rec_itr->second );
            }

            void withdraw_to_transaction( signed_transaction& trx, 
                                          const asset& amount, std::unordered_set<address>& required_sigs )
            { try {
                asset total_in( 0, amount.asset_id );
                asset total_left = amount;
                for( auto record : _accounts )
                {
                   if( record.second.condition.asset_id == amount.asset_id )
                   {
                      if( record.second.balance > 0 )
                      {
                         required_sigs.insert( record.second.condition.as<withdraw_with_signature>().owner );

                         auto withdraw_amount = std::min( record.second.balance, total_left.amount );
                         record.second.balance -= withdraw_amount;

                         ilog( "withdraw amount ${a} ${total_left}", 
                               ("a",withdraw_amount)("total_left",total_left) );
                         trx.withdraw( record.first, withdraw_amount );
                         total_left.amount -= withdraw_amount;

                         if( total_left.amount == 0 ) return;
                      }
                   }
                }
                if( total_left.amount > 0 ) FC_ASSERT( !"Insufficient Funds" );
            } FC_RETHROW_EXCEPTIONS( warn, "", ("amount", amount) ) }

            fc::ecc::private_key get_private_key( const hkey_index& index )
            { try {
                if( index.address_num < 0 )
                {
                    auto priv_key_rec_itr = _extra_receive_keys.find( index.address_num );
                    if( priv_key_rec_itr == _extra_receive_keys.end() )
                    {
                        FC_ASSERT( !"Unable to find private key for address" );
                    }
                    return priv_key_rec_itr->second.get_private_key( _wallet_password );
                }
                else
                {
                    auto priv_key = _master_key->get_extended_private_key( _wallet_password );
                    return priv_key.child( index.contact_num, extended_private_key::private_derivation )
                                   .child( index.trx_num, extended_private_key::public_derivation )
                                   .child( index.address_num, extended_private_key::public_derivation );

                }
            } FC_RETHROW_EXCEPTIONS( warn, "", ("index",index) ) }

            fc::ecc::private_key get_private_key( const address& pub_key_addr )
            { try {

                auto key_index_itr = _receive_keys.find( pub_key_addr );
                FC_ASSERT( key_index_itr != _receive_keys.end() );
                return get_private_key( key_index_itr->second );

            } FC_RETHROW_EXCEPTIONS( warn, "", ("address",pub_key_addr) ) }


            void sign_transaction( signed_transaction& trx, const std::unordered_set<address>& required_sigs )
            { try {
                for( auto item : required_sigs )
                {
                   auto priv_key = get_private_key( item );
                   trx.sign( priv_key );
                }
            } FC_RETHROW_EXCEPTIONS( warn, "", ("trx",trx)("required",required_sigs) ) }

      }; // wallet_impl

   } // namespace detail

   wallet::wallet( const chain_database_ptr& chain_db )
   :my( new detail::wallet_impl() )
   {
      my->self = this;
      my->_blockchain = chain_db;
   }

   wallet::~wallet(){}


   void wallet::open( const fc::path& wallet_dir, const std::string& password )
   { try {
      my->_wallet_db.open( wallet_dir, true );
      unlock(password);

      auto record_itr = my->_wallet_db.begin();
      while( record_itr.valid() )
      {
         auto record = record_itr.value();
         switch( (wallet_record_type)record.type )
         {
            case master_key_record_type:
            {
               my->_master_key = record.as<master_key_record>();
               break;
            }
            case contact_record_type:
            {
               auto cr = record.as<wallet_contact_record>();
               FC_ASSERT( cr.index == record_itr.key() );
               my->_contacts[cr.index] = cr;
               my->_contact_name_index[cr.name] = cr.index;

               if( my->_last_contact_index < cr.index )
                  my->_last_contact_index = cr.index;
               break;
            }
            case transaction_record_type: 
            {
               auto wtr    = record.as<wallet_transaction_record>();
               auto trx_id = wtr.state.trx.id();
               my->_transactions[trx_id] = wtr;
               break;
            }
            case name_record_type:
            {
               auto wnr = record.as<wallet_name_record>();
               my->_names[wnr.id] = wnr;
               break;
            }
            case asset_record_type:
            {
               auto wnr = record.as<wallet_asset_record>();
               my->_assets[wnr.id] = wnr;
               break;
            }
            case account_record_type:
            {
               auto war = record.as<wallet_account_record>();
               my->_accounts[war.id()] = war;
               break;
            }
            case private_key_record_type:
            {
               auto pkr = record.as<private_key_record>();
               my->_extra_receive_keys[pkr.index] = pkr;
               my->_receive_keys[ pkr.get_private_key(my->_wallet_password).get_public_key() ] = 
                     hkey_index( pkr.contact_index, 0, pkr.index );
               break;
            }
            case meta_record_type:
            {
               auto metar = record.as<wallet_meta_record>();
               my->_meta[metar.key] = metar;
               break;
            }
         }
         ++record_itr;
      }
      if( !my->_master_key )
      {
         wlog( "No master key record found, initializing new wallet" );
         my->initialize_wallet();
      }
      my->_current_fee = my->get_default_fee();

   } FC_RETHROW_EXCEPTIONS( warn, "unable to open wallet '${file}'", ("file",wallet_dir) ) }

   void wallet::lock()
   {
      my->_wallet_password = fc::sha512();
   }

   bool wallet::is_unlocked()const
   {
      return my->_wallet_password != fc::sha512();
   }

   void wallet::close()
   { try {
      my->_wallet_db.close();
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::unlock( const std::string& password )
   { try {
      FC_ASSERT( password.size() > 0 );
      my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }


   wallet_contact_record wallet::create_contact( const std::string& name, 
                                                 const extended_public_key& contact_pub_key )
   { try {
        auto current_itr = my->_contact_name_index.find( name );
        FC_ASSERT( current_itr == my->_contact_name_index.end() );
        FC_ASSERT( is_unlocked() );

        wallet_contact_record wcr;
        wcr.index             = ++my->_last_contact_index;
        wcr.name              = name;
        wcr.extended_send_key = contact_pub_key;


        auto master_key = my->_master_key->get_extended_private_key(my->_wallet_password);
        wcr.extended_recv_key = master_key.child( wcr.index );

        my->_contact_name_index[name] = wcr.index;
        my->_contacts[wcr.index] = wcr;
        my->_wallet_db.store( wcr.index, wallet_record(wcr) );
        return wcr;
   } FC_RETHROW_EXCEPTIONS( warn, "unable to create contact", ("name",name)("ext_pub_key", contact_pub_key) ) }

   void wallet::set_contact_extended_send_key( const std::string& name, 
                                               const extended_public_key& contact_pub_key )
   { try {
        auto current_itr = my->_contact_name_index.find(name);
        FC_ASSERT( current_itr != my->_contact_name_index.end() );
        auto current_wcr = my->_contacts.find( current_itr->second );
        FC_ASSERT( current_wcr != my->_contacts.end() );
        current_wcr->second.extended_send_key = contact_pub_key;

        my->_wallet_db.store( current_wcr->second.index, wallet_record(current_wcr->second) );
   } FC_RETHROW_EXCEPTIONS( warn, "unable to create contact", ("name",name)("ext_pub_key", contact_pub_key) ) }

   std::vector<std::string> wallet::get_contacts()const
   {
      std::vector<std::string> cons;
      cons.reserve( my->_contact_name_index.size() );
      for( auto item : my->_contact_name_index )
         cons.push_back( item.first );
      return cons;
   }

   void wallet::import_private_key( const fc::ecc::private_key& priv_key, int32_t contact_index )
   {
      FC_ASSERT( is_unlocked() );
      address pub_address( priv_key.get_public_key() );
      if( my->_receive_keys.find( pub_address ) != my->_receive_keys.end() )
      {
         wlog( "duplicate import of key ${a}", ("a",pub_address) );
         return;
      }
      int32_t key_num = -(EXTRA_PRIVATE_KEY_BASE + my->_extra_receive_keys.size());
      auto pkr = private_key_record( key_num, contact_index, priv_key, my->_wallet_password );
      my->_extra_receive_keys[key_num] = pkr;
      my->_receive_keys[pub_address] = hkey_index( contact_index, 0, key_num );

      my->_wallet_db.store( key_num, pkr );
   }

   void wallet::scan_state()
   {
      my->_blockchain->scan_accounts( [=]( const account_record& rec )
      {
          std::cout << std::string(rec.id()) << "  " << rec.balance << "\n";
          my->scan_account( rec );
      });
      my->_blockchain->scan_names( [=]( const name_record& rec )
      {
          my->scan_name( rec );
      });
   }

   void wallet::scan( const block_summary& summary )
   {
      for( auto account : summary.applied_changes->accounts )
      {
         my->scan_account( account.second );         
      }
      for( auto current_asset : summary.applied_changes->assets )
      {
         my->scan_asset( current_asset.second );
      }
      for( auto name : summary.applied_changes->names )
      {
         my->scan_name( name.second );
      }
      for( auto trx_state : summary.transaction_states )
      {
         my->scan_transaction_state( trx_state );
      }
   }
   asset wallet::get_balance( asset_id_type asset_id  )
   {
      asset balance(0,asset_id);
      for( auto item : my->_accounts )
         balance += item.second.get_balance( asset_id );
      return balance;
   }

   wallet_contact_record wallet::get_contact( uint32_t contact_num )
   { try {
      auto itr = my->_contacts.find( contact_num );
      FC_ASSERT( itr != my->_contacts.end() );
      return itr->second;
   } FC_RETHROW_EXCEPTIONS( warn, "unable to find contact", ("contact_num",contact_num) ) }

   address  wallet::get_new_address( const std::string& name )
   { try {
      return address( get_new_public_key( name ) );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name) ) }

   public_key_type  wallet::get_new_public_key( const std::string& name )
   { try {
      FC_ASSERT( is_unlocked() );
      auto contact_name_itr = my->_contact_name_index.find( name );
      if( contact_name_itr != my->_contact_name_index.end() )
      {
         auto contact = get_contact( contact_name_itr->second );
         auto hindex  = contact.get_next_receive_key_index( 0 );
         wlog( "hindex: ${h}", ("h",hindex) );
         my->_contacts[ contact_name_itr->second ] = contact;
         my->_wallet_db.store( contact.index, contact );

         auto priv_key = my->get_private_key( hindex );
         auto pub_key  = priv_key.get_public_key();
         address pub_addr(pub_key);
         my->_receive_keys[ pub_addr ] = hindex;

         return pub_key;
      }
      else
      {
         wlog( "create contact for '${name}'", ("name",name) );
         create_contact( name );
         return get_new_public_key( name );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name) ) }

   signed_transaction wallet::send_to_address( const asset& amount, const address& owner, const std::string& memo )
   { try {
     FC_ASSERT( is_unlocked() );

     std::unordered_set<address> required_sigs;

     signed_transaction trx;
     if( amount.asset_id == my->_current_fee.asset_id )
     {
        my->withdraw_to_transaction( trx, amount + my->_current_fee, required_sigs );
     }
     else
     {
        my->withdraw_to_transaction( trx, amount, required_sigs );
        my->withdraw_to_transaction( trx, my->_current_fee, required_sigs );
     }

     name_id_type delegate_id = rand()%BTS_BLOCKCHAIN_DELEGATES + 1;
     trx.deposit( owner, amount, delegate_id );
     my->sign_transaction( trx, required_sigs );

     return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("amount",amount)("owner",owner) ) }


   signed_transaction wallet::update_name( const std::string& name, 
                                           fc::optional<fc::variant> json_data, 
                                           fc::optional<public_key_type> active, 
                                           bool as_delegate )
   { try {
      auto name_rec = my->_blockchain->get_name_record( name );
      FC_ASSERT( !!name_rec );

      std::unordered_set<address> required_sigs;
      signed_transaction trx;

      asset total_fees = my->_current_fee;

      if( !!active && *active != name_rec->active_key ) 
         required_sigs.insert( name_rec->owner_key );
      else
         required_sigs.insert( name_rec->active_key );

      auto current_fee_rate = my->_blockchain->get_fee_rate();

      fc::optional<std::string> ojson_str;
      if( !!json_data )
      {
         std::string json_str = fc::json::to_string( *json_data );
         FC_ASSERT( json_str.size() < BTS_BLOCKCHAIN_MAX_NAME_DATA_SIZE );
         FC_ASSERT( name_record::is_valid_json( json_str ) );
         total_fees += asset( (json_str.size() * current_fee_rate)/1000, 0 );
         ojson_str = json_str;
      }
      if( !as_delegate && name_rec->is_delegate )
         FC_ASSERT( !"You cannot unregister as a delegate" );

      if( !name_rec->is_delegate && as_delegate )
      {
        total_fees += asset( (BTS_BLOCKCHAIN_DELEGATE_REGISTRATION_FEE*current_fee_rate)/1000, 0 );
      }

      my->withdraw_to_transaction( trx, total_fees, required_sigs );
      trx.update_name( name_rec->id, ojson_str, active, as_delegate );
      my->sign_transaction( trx, required_sigs );

      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name)("json",json_data)("active",active)("as_delegate",as_delegate) ) }

   signed_transaction wallet::reserve_name( const std::string& name, const fc::variant& json_data, bool as_delegate )
   { try {
      FC_ASSERT( name_record::is_valid_name( name ), "", ("name",name) );
      auto name_rec = my->_blockchain->get_name_record( name );
      FC_ASSERT( !name_rec, "Name '${name}' has already been reserved", ("name",name)  );

      std::unordered_set<address> required_sigs;
      signed_transaction trx;

      asset total_fees = my->_current_fee;

      auto current_fee_rate = my->_blockchain->get_fee_rate();
      if( as_delegate )
      {
        total_fees += asset( (BTS_BLOCKCHAIN_DELEGATE_REGISTRATION_FEE*current_fee_rate)/1000, 0 );
      }
      auto json_str = fc::json::to_string(json_data);
      total_fees += asset( (json_str.size() * current_fee_rate)/1000, 0 );

      my->withdraw_to_transaction( trx, total_fees, required_sigs );

      auto owner_key  = get_new_public_key();
      auto active_key = get_new_public_key();
      trx.reserve_name( name, json_str, owner_key, active_key );

      my->sign_transaction( trx, required_sigs );

      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("name",name)("data",json_data)("delegate",as_delegate) ) }

   bool wallet::is_my_address( const address& addr )const
   {
      auto itr = my->_receive_keys.find( addr );
      return itr != my->_receive_keys.end();
   }

   void wallet::sign_block( signed_block_header& header )const
   { try {
      FC_ASSERT( is_unlocked() );
      auto delegate_pub_key = my->_blockchain->get_signing_delegate_key( header.timestamp );
      auto delegate_key = my->get_private_key( delegate_pub_key );

      header.sign(delegate_key);

   } FC_RETHROW_EXCEPTIONS( warn, "", ("header",header) ) }

   /**
    *  If this wallet is a delegate, calculate the time that it should produce the next 
    *  block.
    *
    *  @see @ref dpos_block_producer
    */
   fc::time_point_sec wallet::next_block_production_time()const
   {
      fc::time_point_sec now = fc::time_point::now();
      int64_t interval_number = now.sec_since_epoch() / BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC;
      int64_t round_start = (interval_number / BTS_BLOCKCHAIN_DELEGATES) * BTS_BLOCKCHAIN_DELEGATES;

      fc::time_point_sec next_time;

      auto sorted_delegates = my->_blockchain->get_delegates_by_vote();
      for( uint32_t i = 0; i < sorted_delegates.size(); ++i )
      {
         auto name_itr = my->_names.find( sorted_delegates[i] );
         if( name_itr != my->_names.end() )
         {
             if( (round_start + i) * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC < now.sec_since_epoch() )
             {
                fc::time_point_sec tmp((round_start + i + BTS_BLOCKCHAIN_DELEGATES) * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC);
                if( tmp < next_time || next_time == fc::time_point_sec() )
                   next_time = tmp;
             }
             else
             {
                fc::time_point_sec tmp((round_start + i) * BTS_BLOCKCHAIN_BLOCK_INTERVAL_SEC);
                if( tmp < next_time || next_time == fc::time_point_sec() )
                   next_time = tmp;
             }
         }
      }
      return next_time;
   }

} } // bts::wallet

#include <roxe.tokenize/roxe.tokenize.hpp>

using namespace std;

namespace roxe {

    void tokenize::create(const name& issuer,
                          const asset& maximum_supply) {
        require_auth(get_self());

        auto sym = maximum_supply.symbol;
        check(sym.is_valid(), "invalid symbol name");
        check(maximum_supply.is_valid(), "invalid supply");
        check(maximum_supply.amount > 0, "max-supply must be positive");

        stats statstable(get_self(), sym.code().raw());
        auto existing = statstable.find(sym.code().raw());
        check(existing == statstable.end(), "token with symbol already exists");

        std:
        vector <name> authors{issuer};
        statstable.emplace(get_self(), [&](auto &s) {
            s.supply.symbol = maximum_supply.symbol;
            s.max_supply = maximum_supply;
            s.issuer = issuer;
            s.authors = authors;

            s.fee = default_tx_fee;
            s.fixed = true;
            s.percent = 0;
            s.maxfee = 0;
            s.minfee = 0;

            s.useroc = true;
        });
    }


    void tokenize::issue(const name& to, const asset& quantity, const string& memo) {
        auto sym = quantity.symbol;
        check(sym.is_valid(), "invalid symbol name");
        check(memo.size() <= 256, "memo has more than 256 bytes");
        check(is_account(to), "to account does not exist");

        stats statstable(get_self(), sym.code().raw());
        auto existing = statstable.find(sym.code().raw());
        check(existing != statstable.end(), "token with symbol does not exist, create token before issue");
        const auto &st = *existing;

        require_recipient(to);

        require_auth(st.issuer);
        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must issue positive quantity");

        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
        check(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

        statstable.modify(st, same_payer, [&](auto &s) {
            s.supply += quantity;
        });

        add_balance(to, quantity, st.issuer);
    }

    void tokenize::retire(const name& from, const asset &quantity, const string &memo) {
        require_auth(from);

        auto sym = quantity.symbol;
        check(sym.is_valid(), "invalid symbol name");
        check(memo.size() <= 256, "memo has more than 256 bytes");

        stats statstable(get_self(), sym.code().raw());
        auto existing = statstable.find(sym.code().raw());
        check(existing != statstable.end(), "token with symbol does not exist");
        const auto &st = *existing;

        std::vector<name>::iterator iter = find(st.authors.begin(), st.authors.end(), from);
        check(iter == st.authors.end(), "retire account from must be authorized");

        require_recipient(from);
//    require_auth( st.issuer );

        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must retire positive quantity");

        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

        statstable.modify(st, same_payer, [&](auto &s) {
            s.supply -= quantity;
        });

        sub_balance(from, quantity);
    }

    void tokenize::transfer(const name &from,
                            const name &to,
                            const asset &quantity,
                            const string &memo) {
        check(from != to, "cannot transfer to self");
        require_auth(from);
        check(is_account(to), "to account does not exist");
        auto sym = quantity.symbol.code();
        stats statstable(get_self(), sym.raw());
        const auto &st = statstable.get(sym.raw());

        require_recipient(from);
        require_recipient(to);

        check(quantity.is_valid(), "invalid quantity");
        check(quantity.amount > 0, "must transfer positive quantity");
        check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
        check(memo.size() <= 256, "memo has more than 256 bytes");

        symbol fee_sym = st.useroc ? symbol(symbol_code("ROC"), 4) : st.supply.symbol;
        int64_t fee_amount = st.fixed ? st.fee : quantity.amount * st.percent / 100;
        if (fee_amount < st.minfee)
            fee_amount = st.minfee;
        if (fee_amount > st.maxfee)
            fee_amount = st.maxfee;

        asset fee = asset(fee_amount, fee_sym);

        auto payer = has_auth(to) ? to : from;

        sub_balance(from, quantity);
        add_balance(to, quantity, payer);
        roxe::name saving_account{"roxe.saving"_n};
        if (st.issuer != to && to != saving_account) {
            sub_balance(from, fee);
            add_balance(saving_account, fee, payer); //FIXME to roxe.system:to_savings
        }

    }

    void tokenize::sub_balance(const name &owner, const asset &value) {
        accounts from_acnts(get_self(), owner.value);

        const auto &from = from_acnts.get(value.symbol.code().raw(), "no balance object found");
        check(from.balance.amount >= value.amount, "overdrawn balance");

        from_acnts.modify(from, owner, [&](auto &a) {
            a.balance -= value;
        });
    }

    void tokenize::add_balance(const name &owner, const asset &value, const name &ram_payer) {
        accounts to_acnts(get_self(), owner.value);
        auto to = to_acnts.find(value.symbol.code().raw());
        if (to == to_acnts.end()) {
            to_acnts.emplace(ram_payer, [&](auto &a) {
                a.balance = value;
            });
        } else {
            to_acnts.modify(to, same_payer, [&](auto &a) {
                a.balance += value;
            });
        }
    }

    void tokenize::open(const name &owner, const symbol &symbol, const name &ram_payer) {
        require_auth(ram_payer);

        check(is_account(owner), "owner account does not exist");

        auto sym_code_raw = symbol.code().raw();
        stats statstable(get_self(), sym_code_raw);
        const auto &st = statstable.get(sym_code_raw, "symbol does not exist");
        check(st.supply.symbol == symbol, "symbol precision mismatch");

        accounts acnts(get_self(), owner.value);
        auto it = acnts.find(sym_code_raw);
        if (it == acnts.end()) {
            acnts.emplace(ram_payer, [&](auto &a) {
                a.balance = asset{0, symbol};
            });
        }
    }

    void tokenize::close(const name &owner, const symbol &symbol) {
        require_auth(owner);
        accounts acnts(get_self(), owner.value);
        auto it = acnts.find(symbol.code().raw());
        check(it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect.");
        check(it->balance.amount == 0, "Cannot close because the balance is not zero.");
        acnts.erase(it);
    }

    void tokenize::setfee(const name &owner, const symbol &symbol, const int64_t fee) {
        require_auth(owner);
        accounts acnts(get_self(), owner.value);
        auto it = acnts.find(symbol.code().raw());
        check(it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect.");
        check(fee >= default_tx_fee, "Cannot set fee below default value(0).");

        stats statstable(get_self(), symbol.code().raw());
        auto existing = statstable.find(symbol.code().raw());
        check(existing != statstable.end(), "token with symbol does not exist, create token before setfee");
        const auto &st = *existing;
        statstable.modify(st, same_payer, [&](auto &s) {
            s.fee = fee;
        });
    }

    void tokenize::addauthor(const symbol& sym, const name& author) {
        require_auth(get_self());

        stats statstable(get_self(), sym.code().raw());
        auto existing = statstable.find(sym.code().raw());
        check(existing != statstable.end(), "token with symbol does not exist");
        const auto &st = *existing;

        statstable.modify(st, same_payer, [&](auto &s) {
            std::vector<name>::iterator iter = find(s.authors.begin(),
                                                         s.authors.end(), author);
            if(iter != s.authors.end())
                s.authors.push_back(author);
        });
    }

    void tokenize::delauthor(const symbol& sym, const name& author) {
        require_auth(get_self());

        stats statstable(get_self(), sym.code().raw());
        auto existing = statstable.find(sym.code().raw());
        check(existing != statstable.end(), "token with symbol does not exist");
        const auto &st = *existing;

        statstable.modify(st, same_payer, [&](auto &s) {
            std::vector<name>::iterator iter = find(s.authors.begin(),
                                                         s.authors.end(), author);
            if(iter == s.authors.end())
                s.authors.erase(iter);;
        });
    }

} /// namespace roxe

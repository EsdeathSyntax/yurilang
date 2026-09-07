#include "yuri/parser.hpp"
#include "yuri/ast.hpp"
#include "yuri/error.hpp"
#include "yuri/logger.hpp"

namespace Yuri {

Parser::Parser(std::vector<Token> t, std::string filename) 
    : tokens(std::move(t)), file_path(std::move(filename)) {
    ErrorReporter::set_file(file_path);
    Logger::log(Subsystem::Parser, LogLevel::Info, "Initialized Parser for file: " + file_path);
}

Token Parser::peek() const { 
    if (current >= tokens.size()) return tokens.back();
    return tokens[current]; 
}

Token Parser::previous() const { 
    if (current == 0) return tokens[0];
    return tokens[current - 1]; 
}

bool Parser::is_at_end() const { return peek().type == TokenType::EndOfFile; }

Token Parser::advance() {
    if (!is_at_end()) current++;
    return previous();
}

bool Parser::check(TokenType type) const {
    if (is_at_end()) return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

std::unique_ptr<AST::Program> Parser::parse() {
    Logger::log(Subsystem::Parser, LogLevel::Info, "Starting AST program parse for: " + file_path);
    auto program = std::make_unique<AST::Program>();
    while (!is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        
        size_t last_token_index = current;

        bool is_module_decl = false;
        if (check(TokenType::KeywordModule) || (check(TokenType::Identifier) && peek().lexeme == "module")) {
            is_module_decl = true;
        } else if (check(TokenType::At)) {
            if (current + 1 < tokens.size() && tokens[current + 1].lexeme == "module") {
                is_module_decl = true;
            }
        }

        if (is_module_decl) {
            if (check(TokenType::At)) {
                advance(); 
            }
            Token mod_keyword = advance(); 
            
            Token name_tok = advance();
            if (name_tok.type != TokenType::Identifier) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected module name identifier at line " + std::to_string(name_tok.line));
                ErrorReporter::error(name_tok.line, name_tok.column, "Expected module name identifier");
            } else {
                program->module_name = name_tok.lexeme;
                Logger::log(Subsystem::Parser, LogLevel::Info, "Parsed module declaration: " + program->module_name);
            }
            match(TokenType::Semicolon);
            continue;
        }

        if (check(TokenType::Hash) || peek().lexeme == "#") {
            Token hash_tok = advance();
            if (is_at_end()) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Unexpected end of file after '#'");
                ErrorReporter::error(hash_tok.line, hash_tok.column, "Unexpected end of file after '#'");
                break;
            }
            
            Token directive = advance();
            if (directive.lexeme == "entry") {
                if (is_at_end()) {
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected function name after #entry directive");
                    ErrorReporter::error(directive.line, directive.column, "Expected function name after #entry directive");
                    break;
                }
                Token target = advance();
                if (target.type != TokenType::Identifier) {
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected function name after #entry directive");
                    ErrorReporter::error(target.line, target.column, "Expected function name after #entry directive, found '" + target.lexeme + "'");
                } else {
                    program->entry_function = target.lexeme;
                    Logger::log(Subsystem::Parser, LogLevel::Info, "Parsed #entry directive targeting function: " + program->entry_function);
                }
            } else {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Unknown preprocessor directive: #" + directive.lexeme);
                ErrorReporter::error(directive.line, directive.column, "Unknown preprocessor directive: #" + directive.lexeme);
            }
            continue;
        }

        if (check(TokenType::KeywordPublic) || check(TokenType::KeywordPrivate)) {
            program->statements.push_back(parse_function());
        } else {
            if (auto stmt = parse_declaration()) {
                program->statements.push_back(std::move(stmt));
            }
        }
        
        if (current == last_token_index) {
            advance();
        }
    }
    Logger::log(Subsystem::Parser, LogLevel::Info, "Successfully finished parsing file: " + file_path);
    return program;
}

std::unique_ptr<AST::Node> Parser::parse_import() {
    Logger::log(Subsystem::Parser, LogLevel::Debug, "Parsing import block");
    match(TokenType::At);
    Token imp_tok = advance();
    if (imp_tok.lexeme != "import") {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected 'import' after '@'");
        ErrorReporter::error(imp_tok.line, imp_tok.column, "Expected 'import' after '@'");
        return nullptr;
    }

    if (!match(TokenType::OpenParen)) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '(' after @import");
        ErrorReporter::error(peek().line, peek().column, "Expected '(' after @import");
        return nullptr;
    }

    auto import_stmt = std::make_unique<AST::ImportStmt>();

    while (match(TokenType::Newline)) {}

    if (!check(TokenType::CloseParen)) {
        do {
            while (match(TokenType::Newline)) {}
            if (check(TokenType::CloseParen)) break;

            bool is_bracketed = match(TokenType::OpenBracket);
            std::vector<std::string> target_names;

            if (is_bracketed) {
                while (match(TokenType::Newline)) {}
                if (!check(TokenType::CloseBracket)) {
                    do {
                        while (match(TokenType::Newline)) {}
                        if (check(TokenType::CloseBracket)) break;

                        if (!check(TokenType::Identifier)) {
                            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected identifier inside import bracket list");
                            ErrorReporter::error(peek().line, peek().column, "Expected identifier inside import bracket list");
                            break;
                        }
                        target_names.push_back(advance().lexeme);

                        match(TokenType::Comma);
                        while (match(TokenType::Newline)) {}
                    } while (!check(TokenType::CloseBracket) && !is_at_end());
                }

                if (!match(TokenType::CloseBracket)) {
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ']' to close import list");
                    ErrorReporter::error(peek().line, peek().column, "Expected ']' to close import list");
                }
            } else {
                if (check(TokenType::Identifier) && peek().lexeme == "type") {
                    advance();
                }

                if (!check(TokenType::Identifier)) {
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected identifier in import list");
                    ErrorReporter::error(peek().line, peek().column, "Expected identifier in import list");
                    break;
                }
                target_names.push_back(advance().lexeme);
            }

            std::string source_mod = "";
            std::string alias = "";

            if (check(TokenType::Identifier)) {
                std::string keyword = peek().lexeme;
                if (keyword == "as" && !is_bracketed) {
                    advance(); 
                    if (check(TokenType::Identifier)) {
                        alias = advance().lexeme;
                    }
                }
                
                if (check(TokenType::Identifier) && peek().lexeme == "from") {
                    advance(); 
                    if (check(TokenType::StringLiteral) || check(TokenType::Identifier)) {
                        source_mod = advance().lexeme;
                    } else {
                        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected module name or path string after 'from'");
                        ErrorReporter::error(peek().line, peek().column, "Expected module name or path string after 'from'");
                    }
                }
            }

            for (const auto& name : target_names) {
                AST::ImportItem item;
                item.name = name;
                item.alias = (target_names.size() == 1) ? alias : "";
                item.source_mod = source_mod;
                import_stmt->items.push_back(item);
            }

            match(TokenType::Comma);
            while (match(TokenType::Newline)) {}
        } while (!check(TokenType::CloseParen) && !is_at_end());
    }

    if (!match(TokenType::CloseParen)) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ')' to close @import block");
        ErrorReporter::error(peek().line, peek().column, "Expected ')' to close @import block");
    }
    match(TokenType::Semicolon); 
    Logger::log(Subsystem::Parser, LogLevel::Debug, "Successfully parsed import block with " + std::to_string(import_stmt->items.size()) + " items");
    return import_stmt;
}

std::unique_ptr<AST::IfStmt> Parser::parse_if_stmt() {
    advance();
    auto stmt = std::make_unique<AST::IfStmt>();

    stmt->condition = parse_expression();

    if (!match(TokenType::OpenBrace)) {
        ErrorReporter::error(peek().line, peek().column, "Expected '{' after if condition");
        return nullptr;
    }

    while (match(TokenType::Newline)) {}

    while (!check(TokenType::CloseBrace) && !is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        if (auto stmt_node = parse_declaration()) {
            stmt->then_branch.push_back(std::move(stmt_node));
        } else {
            advance();
        }
    }

    if (!match(TokenType::CloseBrace)) {
        ErrorReporter::error(peek().line, peek().column, "Expected '}' to close if block");
    }

    if (check(TokenType::Identifier) && peek().lexeme == "else") {
        advance();
        
        if (!match(TokenType::OpenBrace)) {
            ErrorReporter::error(peek().line, peek().column, "Expected '{' after else");
            return stmt;
        }

        while (match(TokenType::Newline)) {}

        while (!check(TokenType::CloseBrace) && !is_at_end()) {
            if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
            if (auto stmt_node = parse_declaration()) {
                stmt->else_branch.push_back(std::move(stmt_node));
            } else {
                advance();
            }
        }

        if (!match(TokenType::CloseBrace)) {
            ErrorReporter::error(peek().line, peek().column, "Expected '}' to close else block");
        }
    }

    return stmt;
}

std::unique_ptr<AST::VarDecl> Parser::parse_var_decl() {
    advance();

    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected variable name after 'let'");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected variable name after 'let'");
    }
    std::string var_name = name_tok.lexeme;
    std::string declared_type = "auto";

    bool is_inferred = (peek().lexeme == ":=" || (check(TokenType::Colon) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "="));

    if (is_inferred) {
        if (peek().lexeme == ":=") {
            advance();
        } else {
            advance(); 
            advance(); 
        }
    } else {
        if (match(TokenType::Colon)) {
            Token type_tok = advance();
            if (type_tok.type != TokenType::Identifier) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected type name after ':'");
                ErrorReporter::error(type_tok.line, type_tok.column, "Expected type name after ':'");
            }
            declared_type = type_tok.lexeme;
            if (match(TokenType::Question)) {
                declared_type += "?";
            }
        }

        if (match(TokenType::Equals) || peek().lexeme == "=") {
            if (peek().lexeme == "=") advance();
        } else {
            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '=' or ':=' in variable declaration");
            ErrorReporter::error(peek().line, peek().column, "Expected assignment operator");
        }
    }

    auto init_expr = parse_expression();
    match(TokenType::Semicolon);
    match(TokenType::Newline);

    auto decl = std::make_unique<AST::VarDecl>();
    decl->type = declared_type;
    decl->name = var_name;
    decl->initializer = std::move(init_expr);
    return decl;
}

std::unique_ptr<AST::WhileStmt> Parser::parse_while_stmt() {
    advance();
    auto stmt = std::make_unique<AST::WhileStmt>();
    stmt->condition = parse_expression();

    if (!match(TokenType::OpenBrace)) {
        ErrorReporter::error(peek().line, peek().column, "Expected '{' after while condition");
        return nullptr;
    }

    while (match(TokenType::Newline)) {}

    while (!check(TokenType::CloseBrace) && !is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        if (auto stmt_node = parse_declaration()) {
            stmt->body.push_back(std::move(stmt_node));
        } else {
            advance();
        }
    }

    if (!match(TokenType::CloseBrace)) {
        ErrorReporter::error(peek().line, peek().column, "Expected '}' to close while block");
    }

    return stmt;
}

std::unique_ptr<AST::Stmt> Parser::parse_for_stmt() {
    advance();

    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected loop variable identifier after 'for'");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected loop variable identifier after 'for'");
        return nullptr;
    }
    std::string var_name = name_tok.lexeme;

    if (check(TokenType::Equals) || peek().lexeme == "=") {
        if (peek().lexeme == "=") advance();
        
        auto stmt = std::make_unique<AST::ForNumericStmt>();
        stmt->var_name = var_name;
        stmt->start = parse_expression();

        if (!match(TokenType::Comma) && peek().lexeme != ",") {
            ErrorReporter::error(peek().line, peek().column, "Expected ',' after numeric loop start value");
            return nullptr;
        }
        if (peek().lexeme == ",") advance();

        stmt->end = parse_expression();

        if (match(TokenType::Comma) || peek().lexeme == ",") {
            if (peek().lexeme == ",") advance();
            stmt->step = parse_expression();
        }

        if (!match(TokenType::OpenBrace)) {
            ErrorReporter::error(peek().line, peek().column, "Expected '{' after numeric for declaration");
            return nullptr;
        }

        while (match(TokenType::Newline)) {}
        while (!check(TokenType::CloseBrace) && !is_at_end()) {
            if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
            if (auto node = parse_declaration()) {
                stmt->body.push_back(std::move(node));
            } else {
                advance();
            }
        }
        match(TokenType::CloseBrace);
        return stmt;
    } 
    else if (check(TokenType::Identifier) && peek().lexeme == "in") {
        advance();

        auto stmt = std::make_unique<AST::ForInStmt>();
        stmt->var_name = var_name;
        stmt->iterable = parse_expression();

        if (!match(TokenType::OpenBrace)) {
            ErrorReporter::error(peek().line, peek().column, "Expected '{' after iterator for declaration");
            return nullptr;
        }

        while (match(TokenType::Newline)) {}
        while (!check(TokenType::CloseBrace) && !is_at_end()) {
            if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
            if (auto node = parse_declaration()) {
                stmt->body.push_back(std::move(node));
            } else {
                advance();
            }
        }
        match(TokenType::CloseBrace);
        return stmt;
    }

    ErrorReporter::error(name_tok.line, name_tok.column, "Expected '=' or 'in' in for loop declaration");
    return nullptr;
}

std::unique_ptr<AST::Node> Parser::parse_declaration() {
    if (check(TokenType::At)) {
        if (current + 1 < tokens.size() && tokens[current + 1].lexeme == "import") {
            return parse_import();
        }
    }

    

    if (check(TokenType::Identifier) && peek().lexeme == "if") {
        return parse_if_stmt();
    }

    if (check(TokenType::Identifier) && peek().lexeme == "for") {
        return parse_for_stmt();
    }

    if (check(TokenType::Identifier) && peek().lexeme == "while") {
        return parse_while_stmt();
    }

    if (check(TokenType::Identifier) && peek().lexeme == "break") {
        advance();
        match(TokenType::Semicolon);
        match(TokenType::Newline);
        return std::make_unique<AST::BreakStmt>();
    }

    if (match(TokenType::KeywordNamespace)) return parse_namespace();
    if (match(TokenType::KeywordClass)) return parse_class();
    if (check(TokenType::KeywordPublic) || check(TokenType::KeywordPrivate)) return parse_function();
    
    if (match(TokenType::KeywordReturn)) {
        auto ret = std::make_unique<AST::ReturnStmt>();
        if (!check(TokenType::Semicolon) && !check(TokenType::Newline) && !check(TokenType::CloseBrace)) {
            ret->value = parse_expression();
        }
        match(TokenType::Semicolon);
        return ret;
    }

    bool is_let = check(TokenType::KeywordLet) || (check(TokenType::Identifier) && peek().lexeme == "let");
    if (is_let) {
        return parse_var_decl();
    }

    bool is_identifier = check(TokenType::Identifier);
    bool has_next = (current + 1 < tokens.size());
    
    if (is_identifier && has_next) {
        TokenType next_type = tokens[current + 1].type;
        bool is_assign = (next_type == TokenType::Equals);
        bool is_compound = (next_type == TokenType::PlusEquals || 
                            next_type == TokenType::MinusEquals || 
                            next_type == TokenType::StarEquals || 
                            next_type == TokenType::SlashEquals);

        if (is_assign || is_compound) {
            std::string var_name = advance().lexeme;
            Token op_tok = advance();
            
            auto rhs_expr = parse_expression();
            match(TokenType::Semicolon);
            match(TokenType::Newline);

            std::unique_ptr<AST::Expr> final_val = std::move(rhs_expr);

            if (is_compound) {
                std::string bin_op;
                if (op_tok.lexeme == "+=") bin_op = "+";
                else if (op_tok.lexeme == "-=") bin_op = "-";
                else if (op_tok.lexeme == "*=") bin_op = "*";
                else if (op_tok.lexeme == "/=") bin_op = "/";

                auto bin_expr = std::make_unique<AST::BinaryExpr>();
                auto var_expr = std::make_unique<AST::VariableExpr>();
                var_expr->name = var_name;
                
                bin_expr->left = std::move(var_expr);
                bin_expr->op = bin_op;
                bin_expr->right = std::move(final_val);
                final_val = std::move(bin_expr);
            }

            auto assign = std::make_unique<AST::AssignExpr>();
            assign->name = var_name;
            assign->value = std::move(final_val);
            return assign;
        }
    }

    auto expr = parse_expression();
    match(TokenType::Semicolon);
    match(TokenType::Newline);
    return expr;
}

std::unique_ptr<AST::Namespace> Parser::parse_namespace() {
    auto ns = std::make_unique<AST::Namespace>();
    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected namespace identifier");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected namespace identifier");
    }
    ns->name = name_tok.lexeme;
    Logger::log(Subsystem::Parser, LogLevel::Debug, "Parsing namespace: " + ns->name);
    
    if (!match(TokenType::OpenBrace)) {
        Token cur = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '{' after namespace declaration");
        ErrorReporter::error(cur.line, cur.column, "Expected '{' after namespace declaration");
    }
    
    while (!match(TokenType::CloseBrace) && !is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        ns->members.push_back(parse_declaration());
    }
    return ns;
}

std::unique_ptr<AST::Class> Parser::parse_class() {
    auto cls = std::make_unique<AST::Class>();
    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected class identifier");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected class identifier");
    }
    cls->name = name_tok.lexeme;
    Logger::log(Subsystem::Parser, LogLevel::Debug, "Parsing class: " + cls->name);

    if (!match(TokenType::OpenBrace)) {
        Token cur = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '{' after class declaration");
        ErrorReporter::error(cur.line, cur.column, "Expected '{' after class declaration");
    }

    while (!match(TokenType::CloseBrace) && !is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        cls->members.push_back(parse_declaration());
    }
    return cls;
}

std::unique_ptr<AST::Function> Parser::parse_function() {
    auto fn = std::make_unique<AST::Function>();
    fn->visibility = advance().lexeme;
    
    if (!match(TokenType::KeywordFn)) {
        Token cur = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected 'fn' keyword in function definition");
        ErrorReporter::error(cur.line, cur.column, "Expected 'fn' keyword in function definition");
    }
    
    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected function name identifier");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected function name identifier");
    }
    fn->name = name_tok.lexeme;
    Logger::log(Subsystem::Parser, LogLevel::Debug, "Parsing function: " + fn->name);
    
    if (!match(TokenType::OpenParen)) {
        Token cur = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '(' after function name");
        ErrorReporter::error(cur.line, cur.column, "Expected '(' after function name");
    }

    if (!check(TokenType::CloseParen)) {
        do {
            Token p_name = advance();
            if (p_name.type != TokenType::Identifier) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected parameter name identifier");
                ErrorReporter::error(p_name.line, p_name.column, "Expected parameter name identifier");
            }
            if (!match(TokenType::Colon)) {
                Token cur = peek();
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ':' after parameter name");
                ErrorReporter::error(cur.line, cur.column, "Expected ':' after parameter name");
            }
            Token p_type_tok = advance();
            std::string p_type = p_type_tok.lexeme;
            if (check(TokenType::Question) || (current < tokens.size() && tokens[current].lexeme == "?")) {
                advance();
                p_type += "?";
            }
            fn->params.push_back({p_name.lexeme, p_type});
        } while (match(TokenType::Comma));
    }
    match(TokenType::CloseParen);

    if (match(TokenType::Arrow)) {
        fn->ret_type = advance().lexeme;
        if (match(TokenType::Question)) {
            fn->ret_type += "?";
        }
    } else {
        fn->ret_type = ""; 
    }
    
    if (!match(TokenType::OpenBrace)) {
        Token cur = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '{' to start function body");
        ErrorReporter::error(cur.line, cur.column, "Expected '{' to start function body");
    }

    while (!match(TokenType::CloseBrace) && !is_at_end()) {
        if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;
        
        if (auto stmt = parse_declaration()) {
            fn->body.push_back(std::move(stmt));
        } else {
            advance();
        }
    }

    if (fn->ret_type.empty()) {
        if (!fn->body.empty()) {
            if (auto ret_stmt = dynamic_cast<AST::ReturnStmt*>(fn->body.back().get())) {
                if (ret_stmt->value) {
                    fn->ret_type = "int";
                } else {
                    fn->ret_type = "void";
                }
            } else {
                fn->ret_type = "void";
            }
        } else {
            fn->ret_type = "void";
        }
    }

    Logger::log(Subsystem::Parser, LogLevel::Debug, "Successfully parsed function: " + fn->name + " -> " + fn->ret_type);
    return fn;
}

std::string Parser::parse_type_string() {
    while (match(TokenType::Newline)) {}
    if (!check(TokenType::Identifier)) {
        ErrorReporter::error(peek().line, peek().column, "Expected type name identifier");
        return "auto";
    }
    std::string type_str = advance().lexeme;
    
    // Support pointers, arrays, or nullables if needed
    if (match(TokenType::Question)) {
        type_str += "?";
    } else if (match(TokenType::Star)) {
        type_str += "*";
    } else if (match(TokenType::OpenBracket)) {
        if (!match(TokenType::CloseBracket)) {
            ErrorReporter::error(peek().line, peek().column, "Expected ']' to close array type");
        }
        type_str += "[]";
    }
    return type_str;
}

std::unique_ptr<AST::Expr> Parser::parse_type_definition() {
    advance();

    while (match(TokenType::Newline)) {}
    Token name_tok = advance();
    if (name_tok.type != TokenType::Identifier) {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected type name identifier after 'type'");
        ErrorReporter::error(name_tok.line, name_tok.column, "Expected type name identifier after 'type'");
        return nullptr;
    }
    std::string type_name = name_tok.lexeme;

    while (match(TokenType::Newline)) {}
    if (!match(TokenType::Equals) && peek().lexeme != "=") {
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '=' in type definition for " + type_name);
        ErrorReporter::error(peek().line, peek().column, "Expected '=' in type definition");
        return nullptr;
    }
    if (peek().lexeme == "=") advance();

    while (match(TokenType::Newline)) {}

    if (match(TokenType::OpenBrace) || peek().lexeme == "{") {
        if (peek().lexeme == "{") advance();
        
        auto struct_decl = std::make_unique<AST::StructTypeDecl>();
        struct_decl->name = type_name;

        while (match(TokenType::Newline)) {}
        while (!check(TokenType::CloseBrace) && !is_at_end()) {
            if (match(TokenType::Semicolon) || match(TokenType::Newline)) continue;

            Token field_tok = advance();
            if (field_tok.type != TokenType::Identifier) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected field name identifier in struct type definition");
                ErrorReporter::error(field_tok.line, field_tok.column, "Expected field name identifier");
                break;
            }
            std::string field_name = field_tok.lexeme;

            while (match(TokenType::Newline)) {}
            if (!match(TokenType::Colon) && peek().lexeme != ":") {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ':' after field name in struct type definition");
                ErrorReporter::error(peek().line, peek().column, "Expected ':' after field name");
            }
            if (peek().lexeme == ":") advance();

            std::string field_type = parse_type_string();
            struct_decl->fields.push_back({field_name, field_type});

            match(TokenType::Comma);
            match(TokenType::Semicolon);
            while (match(TokenType::Newline)) {}
        }

        if (!match(TokenType::CloseBrace) && peek().lexeme != "}") {
            ErrorReporter::error(peek().line, peek().column, "Expected '}' to close struct type definition");
        }
        if (peek().lexeme == "}") advance();

        match(TokenType::Semicolon);
        match(TokenType::Newline);
        return struct_decl;
    } 
    else {
        auto alias_decl = std::make_unique<AST::TypeAliasDecl>();
        alias_decl->name = type_name;
        alias_decl->target_type = parse_type_string();

        match(TokenType::Semicolon);
        match(TokenType::Newline);
        return alias_decl;
    }
}


std::shared_ptr<Yuri::Type> Parser::parse_type() {
    if (!check(TokenType::Identifier)) {
        ErrorReporter::error(peek().line, peek().column, "Expected type identifier");
        return nullptr;
    }
    
    Token type_token = advance();
    std::string type_name = type_token.lexeme;

    Yuri::TypeKind kind = Yuri::TypeKind::Custom;
    if (type_name == "void") kind = Yuri::TypeKind::Void;
    else if (type_name == "i64") kind = Yuri::TypeKind::Int64;
    else if (type_name == "i32") kind = Yuri::TypeKind::Int32;
    else if (type_name == "bool") kind = Yuri::TypeKind::Bool;
    else if (type_name == "f32") kind = Yuri::TypeKind::Float32;
    else if (type_name == "f64") kind = Yuri::TypeKind::Float64;
    else if (type_name == "str") kind = Yuri::TypeKind::String;

    auto base_type = Yuri::Type::make(kind, kind == Yuri::TypeKind::Custom ? type_name : "");

    if (match(TokenType::Question)) {
        base_type = Yuri::Type::make_array(base_type);
    }

    return base_type;
}

std::unique_ptr<AST::Expr> Parser::parse_expression() {
    return parse_comparison();
}

std::unique_ptr<AST::Expr> Parser::parse_comparison() {
    auto expr = parse_string_concat();
    while (match(TokenType::Match) || match(TokenType::NotEqual) || 
           match(TokenType::Less) || match(TokenType::LessEqual) || 
           match(TokenType::Greater) || match(TokenType::GreaterEqual)) {
        std::string op = previous().lexeme;
        auto right = parse_string_concat();
        auto binary = std::make_unique<AST::BinaryExpr>();
        binary->left = std::move(expr);
        binary->op = op;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<AST::Expr> Parser::parse_string_concat() {
    auto expr = parse_bitwise();
    while (match(TokenType::DotDot)) {
        auto right = parse_bitwise();
        auto binary = std::make_unique<AST::BinaryExpr>();
        binary->left = std::move(expr);
        binary->op = "..";
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<AST::Expr> Parser::parse_additive() {
    auto expr = parse_factor();
    while (match(TokenType::Plus) || match(TokenType::Minus)) {
        std::string op = previous().lexeme;
        auto right = parse_factor();
        auto binary = std::make_unique<AST::BinaryExpr>();
        binary->left = std::move(expr);
        binary->op = op;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<AST::Expr> Parser::parse_bitwise() {
    auto expr = parse_additive();
    while (match(TokenType::BitshiftL) || match(TokenType::BitshiftR) || 
           match(TokenType::BitAnd) || match(TokenType::BitOr) || match(TokenType::BitXor)) {
        std::string op = previous().lexeme;
        auto right = parse_additive();
        auto binary = std::make_unique<AST::BinaryExpr>();
        binary->left = std::move(expr);
        binary->op = op;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<AST::Expr> Parser::parse_factor() {
    auto expr = parse_primary();
    while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
        std::string op = previous().lexeme;
        auto right = parse_primary();
        auto binary = std::make_unique<AST::BinaryExpr>();
        binary->left = std::move(expr);
        binary->op = op;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<AST::Expr> Parser::parse_primary() {
    std::unique_ptr<AST::Expr> expr = nullptr;

    bool is_type_decl = check(TokenType::KeywordType) || (check(TokenType::Identifier) && peek().lexeme == "type");
    if (is_type_decl) {
        return parse_type_definition();
    }

    if (match(TokenType::OpenBracket)) {
        auto arr_expr = std::make_unique<AST::ArrayLiteralExpr>();
        if (!check(TokenType::CloseBracket)) {
            do {
                arr_expr->elements.push_back(parse_expression());
            } while (match(TokenType::Comma));
        }
        if (!match(TokenType::CloseBracket)) {
            Token cur = peek();
            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ']' after array literal");
            ErrorReporter::error(cur.line, cur.column, "Expected ']' after array literal");
        }
        expr = std::move(arr_expr);
    }
    else if (match(TokenType::OpenBrace)) {
        auto tbl_expr = std::make_unique<AST::TableLiteralExpr>();
        if (!check(TokenType::CloseBrace)) {
            do {
                Token key_tok = peek();
                if (key_tok.type != TokenType::Identifier) {
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected table key identifier");
                    ErrorReporter::error(key_tok.line, key_tok.column, "Expected table key identifier");
                    break;
                }
                std::string key = advance().lexeme;

                if (!match(TokenType::Equals)) {
                    Token cur = peek();
                    Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '=' after table key");
                    ErrorReporter::error(cur.line, cur.column, "Expected '=' after table key");
                }

                auto val = parse_expression();
                tbl_expr->entries.push_back({key, std::move(val)});
            } while (match(TokenType::Comma) || match(TokenType::Semicolon) || match(TokenType::Newline));
        }
        if (!match(TokenType::CloseBrace)) {
            Token cur = peek();
            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected '}' after table literal");
            ErrorReporter::error(cur.line, cur.column, "Expected '}' after table literal");
        }
        expr = std::move(tbl_expr);
    }
    else if (match(TokenType::StringLiteral)) {
        auto str_expr = std::make_unique<AST::StringLiteralExpr>();
        str_expr->value = previous().lexeme;
        expr = std::move(str_expr);
    }
    else if (match(TokenType::Number)) {
        std::string lexeme = previous().lexeme;
        if (lexeme.find('.') != std::string::npos) {
            auto f_expr = std::make_unique<AST::FloatLiteralExpr>(); 
            f_expr->value = std::stod(lexeme);
            expr = std::move(f_expr);
        } else {
            auto l_expr = std::make_unique<AST::LiteralExpr>();
            l_expr->value = std::stoll(lexeme, nullptr, 0); 
            expr = std::move(l_expr);
        }
    }
    else if (check(TokenType::Identifier) && peek().lexeme == "nullptr") {
        advance();
        expr = std::make_unique<AST::NullPtrExpr>();
    }
    else if (check(TokenType::Identifier) && peek().lexeme == "true") {
        advance();
        auto b_expr = std::make_unique<AST::BoolLiteralExpr>();
        b_expr->value = true;
        expr = std::move(b_expr);
    }
    else if (check(TokenType::Identifier) && peek().lexeme == "false") {
        advance();
        auto b_expr = std::make_unique<AST::BoolLiteralExpr>();
        b_expr->value = false;
        expr = std::move(b_expr);
    }
    else if (check(TokenType::Identifier)) {
        Token id_token = advance();
        std::string name = id_token.lexeme;
        std::string module_name = "";

        if (match(TokenType::Dot)) {
            module_name = name;
            Token method_tok = peek();
            if (method_tok.type != TokenType::Identifier) {
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected member name after '.'");
                ErrorReporter::error(method_tok.line, method_tok.column, "Expected member name after '.'");
            } else {
                advance();
                name = method_tok.lexeme;
            }
        }

        if (match(TokenType::OpenParen)) {
            auto call = std::make_unique<AST::CallExpr>();
            call->module = module_name;
            call->method = name;
            if (!check(TokenType::CloseParen)) {
                do {
                    call->arguments.push_back(parse_expression());
                } while (match(TokenType::Comma));
            }
            if (!match(TokenType::CloseParen)) {
                Token cur = peek();
                Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ')' after function arguments");
                ErrorReporter::error(cur.line, cur.column, "Expected ')' after function arguments");
            }
            expr = std::move(call);
        } else {
            auto var_expr = std::make_unique<AST::VariableExpr>();
            if (!module_name.empty()) {
                var_expr->name = module_name + "." + name;
            } else {
                var_expr->name = name;
            }
            expr = std::move(var_expr);
        }
    }
    else if (match(TokenType::OpenParen)) {
        expr = parse_expression();
        if (!match(TokenType::CloseParen)) {
            Token cur = previous();
            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ')' after grouped expression");
            ErrorReporter::error(cur.line, cur.column, "Expected ')' after grouped expression");
        }
    }
    else {
        auto current_token = peek();
        Logger::log(Subsystem::Parser, LogLevel::Error, "Expected expression, found '" + current_token.lexeme + "'");
        ErrorReporter::error(current_token.line, current_token.column, "Expected expression, found '" + current_token.lexeme + "'");
        advance();
        return nullptr;
    }

    while (match(TokenType::OpenBracket)) {
        auto index_expr = std::make_unique<AST::IndexExpr>();
        index_expr->target = std::move(expr);
        index_expr->index = parse_expression();
        
        if (!match(TokenType::CloseBracket)) {
            Token cur = peek();
            Logger::log(Subsystem::Parser, LogLevel::Error, "Expected ']' after index expression");
            ErrorReporter::error(cur.line, cur.column, "Expected ']' after index expression");
        }
        expr = std::move(index_expr);
    }

    return expr;
}

} // namespace Yuri
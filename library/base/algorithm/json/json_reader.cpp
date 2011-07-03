
#include "json_reader.h"

#include "base/float_util.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/value.h"

namespace base
{

    static const JSONReader::Token kInvalidToken(
        JSONReader::Token::INVALID_TOKEN, 0, 0);
    static const int kStackLimit = 100;

    namespace
    {

        // ParseNumberToken�ĸ�������. ��token��ȡһ��int. û�кϷ�������������false.
        bool ReadInt(JSONReader::Token& token, bool can_have_leading_zeros)
        {
            wchar_t first = token.NextChar();
            int len = 0;

            // ��ȡ��������.
            wchar_t c = first;
            while('\0'!=c && '0'<=c && c<='9')
            {
                ++token.length;
                ++len;
                c = token.NextChar();
            }
            // ������Ҫ1������.
            if(len == 0)
            {
                return false;
            }

            if(!can_have_leading_zeros && len>1 && '0'==first)
            {
                return false;
            }

            return true;
        }

        // ParseStringToken�ĸ�������. ��token��ȡ|digits|��16��������, �в��Ϸ�����
        // (�����ַ�), ��������false.
        bool ReadHexDigits(JSONReader::Token& token, int digits)
        {
            for(int i=1; i<=digits; ++i)
            {
                wchar_t c = *(token.begin + token.length + i);
                if('\0' == c)
                {
                    return false;
                }
                if(!(('0'<=c && c<='9') || ('a'<=c && c<='f') ||
                    ('A'<=c && c<='F')))
                {
                    return false;
                }
            }

            token.length += digits;
            return true;
        }

    }

    const char* JSONReader::kBadRootElementType =
        "Root value must be an array or object.";
    const char* JSONReader::kInvalidEscape =
        "Invalid escape sequence.";
    const char* JSONReader::kSyntaxError =
        "Syntax error.";
    const char* JSONReader::kTrailingComma =
        "Trailing comma not allowed.";
    const char* JSONReader::kTooMuchNesting =
        "Too much nesting.";
    const char* JSONReader::kUnexpectedDataAfterRoot =
        "Unexpected data after root element.";
    const char* JSONReader::kUnsupportedEncoding =
        "Unsupported encoding. JSON must be UTF-8.";
    const char* JSONReader::kUnquotedDictionaryKey =
        "Dictionary keys must be quoted.";

    /* static */
    Value* JSONReader::Read(const std::string& json,
        bool allow_trailing_comma)
    {
        return ReadAndReturnError(json, allow_trailing_comma, NULL, NULL);
    }

    /* static */
    Value* JSONReader::ReadAndReturnError(const std::string& json,
        bool allow_trailing_comma, int* error_code_out,
        std::string* error_msg_out)
    {
        JSONReader reader = JSONReader();
        Value* root = reader.JsonToValue(json, true, allow_trailing_comma);
        if(root)
        {
            return root;
        }

        if(error_code_out)
        {
            *error_code_out = reader.error_code();
        }
        if(error_msg_out)
        {
            *error_msg_out = reader.GetErrorMessage();
        }

        return NULL;
    }

    /* static */
    std::string JSONReader::FormatErrorMessage(int line, int column,
        const std::string& description)
    {
        if(line || column)
        {
            return StringPrintf("Line: %i, column: %i, %s",
                line, column, description.c_str());
        }
        return description;
    }

    /* static */
    std::string JSONReader::ErrorCodeToString(JsonParseError error_code)
    {
        switch(error_code)
        {
        case JSON_NO_ERROR:
            return std::string();
        case JSON_BAD_ROOT_ELEMENT_TYPE:
            return kBadRootElementType;
        case JSON_INVALID_ESCAPE:
            return kInvalidEscape;
        case JSON_SYNTAX_ERROR:
            return kSyntaxError;
        case JSON_TRAILING_COMMA:
            return kTrailingComma;
        case JSON_TOO_MUCH_NESTING:
            return kTooMuchNesting;
        case JSON_UNEXPECTED_DATA_AFTER_ROOT:
            return kUnexpectedDataAfterRoot;
        case JSON_UNSUPPORTED_ENCODING:
            return kUnsupportedEncoding;
        case JSON_UNQUOTED_DICTIONARY_KEY:
            return kUnquotedDictionaryKey;
        default:
            NOTREACHED();
            return std::string();
        }
    }

    std::string JSONReader::GetErrorMessage() const
    {
        return FormatErrorMessage(error_line_, error_col_,
            ErrorCodeToString(error_code_));
    }

    JSONReader::JSONReader() : start_pos_(NULL), json_pos_(NULL),
        stack_depth_(0), allow_trailing_comma_(false),
        error_code_(JSON_NO_ERROR), error_line_(0), error_col_(0) {}

    Value* JSONReader::JsonToValue(const std::string& json, bool check_root,
        bool allow_trailing_comma)
    {
        // ���������UTF-8����.
        if(!IsStringUTF8(json.c_str()))
        {
            error_code_ = JSON_UNSUPPORTED_ENCODING;
            return NULL;
        }

        // ��UTF8��wstring��ת�����Ƴ����ֽ�(����).
        std::wstring json_wide(UTF8ToWide(json));
        start_pos_ = json_wide.c_str();

        // �������JSON�ַ�����ͷ��UTF-8��Byte-Order-Mark(0xEF, 0xBB, 0xBF),
        // UTF8ToWide()���������ת����BOM(U+FEFF). Ϊ��ֹJSONReader::BuildValue()
        // �����������ɷǷ��ַ�������NULL, �������Unicode��BOM��������.
        if(!json_wide.empty() && start_pos_[0]==0xFEFF)
        {
            ++start_pos_;
        }

        json_pos_ = start_pos_;
        allow_trailing_comma_ = allow_trailing_comma;
        stack_depth_ = 0;
        error_code_ = JSON_NO_ERROR;

        scoped_ptr<Value> root(BuildValue(check_root));
        if(root.get())
        {
            if(ParseToken().type == Token::END_OF_INPUT)
            {
                return root.release();
            }
            else
            {
                SetErrorCode(JSON_UNEXPECTED_DATA_AFTER_ROOT, json_pos_);
            }
        }

        // "�﷨����".
        if(error_code_ == 0)
        {
            SetErrorCode(JSON_SYNTAX_ERROR, json_pos_);
        }

        return NULL;
    }

    Value* JSONReader::BuildValue(bool is_root)
    {
        ++stack_depth_;
        if(stack_depth_ > kStackLimit)
        {
            SetErrorCode(JSON_TOO_MUCH_NESTING, json_pos_);
            return NULL;
        }

        Token token = ParseToken();
        // ��token������������߶���.
        if(is_root && token.type!=Token::OBJECT_BEGIN &&
            token.type!=Token::ARRAY_BEGIN)
        {
            SetErrorCode(JSON_BAD_ROOT_ELEMENT_TYPE, json_pos_);
            return NULL;
        }

        scoped_ptr<Value> node;

        switch(token.type)
        {
        case Token::END_OF_INPUT:
        case Token::INVALID_TOKEN:
            return NULL;

        case Token::NULL_TOKEN:
            node.reset(Value::CreateNullValue());
            break;

        case Token::BOOL_TRUE:
            node.reset(Value::CreateBooleanValue(true));
            break;

        case Token::BOOL_FALSE:
            node.reset(Value::CreateBooleanValue(false));
            break;

        case Token::NUMBER:
            node.reset(DecodeNumber(token));
            if(!node.get())
            {
                return NULL;
            }
            break;

        case Token::STRING:
            node.reset(DecodeString(token));
            if(!node.get())
            {
                return NULL;
            }
            break;

        case Token::ARRAY_BEGIN:
            {
                json_pos_ += token.length;
                token = ParseToken();

                node.reset(new ListValue());
                while(token.type != Token::ARRAY_END)
                {
                    Value* array_node = BuildValue(false);
                    if(!array_node)
                    {
                        return NULL;
                    }
                    static_cast<ListValue*>(node.get())->Append(array_node);

                    // list���ݺ���Ӧ���Ƕ���, ����list����.
                    token = ParseToken();
                    if(token.type == Token::LIST_SEPARATOR)
                    {
                        json_pos_ += token.length;
                        token = ParseToken();
                        // ����JSON RFC��β�Ķ����ǲ��Ϸ���, ������Щ��ϣ���ܿ���һЩ,
                        // ������һЩ��Ӧ����.
                        if(token.type == Token::ARRAY_END)
                        {
                            if(!allow_trailing_comma_)
                            {
                                SetErrorCode(JSON_TRAILING_COMMA, json_pos_);
                                return NULL;
                            }
                            // ��β�ж���, ֹͣ��������Array.
                            break;
                        }
                    }
                    else if(token.type != Token::ARRAY_END)
                    {
                        // ��Ԥ������, ֱ�ӷ���.
                        return NULL;
                    }
                }
                if(token.type != Token::ARRAY_END)
                {
                    return NULL;
                }
                break;
            }

        case Token::OBJECT_BEGIN:
            {
                json_pos_ += token.length;
                token = ParseToken();

                node.reset(new DictionaryValue());
                while(token.type != Token::OBJECT_END)
                {
                    if(token.type != Token::STRING)
                    {
                        SetErrorCode(JSON_UNQUOTED_DICTIONARY_KEY, json_pos_);
                        return NULL;
                    }
                    scoped_ptr<Value> dict_key_value(DecodeString(token));
                    if(!dict_key_value.get())
                    {
                        return NULL;
                    }

                    // keyת����wstring.
                    std::string dict_key;
                    bool success = dict_key_value->GetAsString(&dict_key);
                    DCHECK(success);

                    json_pos_ += token.length;
                    token = ParseToken();
                    if(token.type != Token::OBJECT_PAIR_SEPARATOR)
                    {
                        return NULL;
                    }

                    json_pos_ += token.length;
                    token = ParseToken();
                    Value* dict_value = BuildValue(false);
                    if(!dict_value)
                    {
                        return NULL;
                    }
                    static_cast<DictionaryValue*>(node.get())->SetWithoutPathExpansion(
                        dict_key, dict_value);

                    // key/value����Ӧ���Ƕ���, ����������.
                    token = ParseToken();
                    if(token.type == Token::LIST_SEPARATOR)
                    {
                        json_pos_ += token.length;
                        token = ParseToken();
                        // ����JSON RFC��β�Ķ����ǲ��Ϸ���, ������Щ��ϣ���ܿ���һЩ,
                        // ������һЩ��Ӧ����.
                        if(token.type == Token::OBJECT_END)
                        {
                            if(!allow_trailing_comma_)
                            {
                                SetErrorCode(JSON_TRAILING_COMMA, json_pos_);
                                return NULL;
                            }
                            // ��β�ж���, ֹͣ��������Array.
                            break;
                        }
                    }
                    else if(token.type != Token::OBJECT_END)
                    {
                        // ��Ԥ������, ֱ�ӷ���.
                        return NULL;
                    }
                }
                if(token.type != Token::OBJECT_END)
                {
                    return NULL;
                }

                break;
            }

        default:
            // ������token.
            return NULL;
        }
        json_pos_ += token.length;

        --stack_depth_;
        return node.release();
    }

    JSONReader::Token JSONReader::ParseNumberToken()
    {
        // ����ֻ��������, ��DecodeNumber������֤. ����RFC4627, �Ϸ�������:
        // [-]int[С������][ָ������].
        Token token(Token::NUMBER, json_pos_, 0);
        wchar_t c = *json_pos_;
        if('-' == c)
        {
            ++token.length;
            c = token.NextChar();
        }

        if(!ReadInt(token, false))
        {
            return kInvalidToken;
        }

        // С�������ǿ�ѡ��.
        c = token.NextChar();
        if('.' == c)
        {
            ++token.length;
            if(!ReadInt(token, true))
            {
                return kInvalidToken;
            }
            c = token.NextChar();
        }

        // ָ�������ǿ�ѡ��.
        if('e'==c || 'E'==c)
        {
            ++token.length;
            c = token.NextChar();
            if('-'==c || '+'==c)
            {
                ++token.length;
                c = token.NextChar();
            }
            if(!ReadInt(token, true))
            {
                return kInvalidToken;
            }
        }

        return token;
    }

    Value* JSONReader::DecodeNumber(const Token& token)
    {
        const std::wstring num_string(token.begin, token.length);

        int num_int;
        if(StringToInt(WideToUTF8(num_string), &num_int))
        {
            return Value::CreateIntegerValue(num_int);
        }

        double num_double;
        if(StringToDouble(WideToUTF8(num_string), &num_double) &&
            base::IsFinite(num_double))
        {
            return Value::CreateDoubleValue(num_double);
        }

        return NULL;
    }

    JSONReader::Token JSONReader::ParseStringToken()
    {
        Token token(Token::STRING, json_pos_, 1);
        wchar_t c = token.NextChar();
        while('\0' != c)
        {
            if('\\' == c)
            {
                ++token.length;
                c = token.NextChar();
                // ȷ��ת�����ȷ.
                switch(c)
                {
                case 'x':
                    if(!ReadHexDigits(token, 2))
                    {
                        SetErrorCode(JSON_INVALID_ESCAPE, json_pos_+token.length);
                        return kInvalidToken;
                    }
                    break;
                case 'u':
                    if(!ReadHexDigits(token, 4))
                    {
                        SetErrorCode(JSON_INVALID_ESCAPE, json_pos_+token.length);
                        return kInvalidToken;
                    }
                    break;
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                case 'v':
                case '"':
                    break;
                default:
                    SetErrorCode(JSON_INVALID_ESCAPE, json_pos_+token.length);
                    return kInvalidToken;
                }
            }
            else if('"' == c)
            {
                ++token.length;
                return token;
            }
            ++token.length;
            c = token.NextChar();
        }
        return kInvalidToken;
    }

    Value* JSONReader::DecodeString(const Token& token)
    {
        std::wstring decoded_str;
        decoded_str.reserve(token.length-2);

        for(int i=1; i<token.length-1; ++i)
        {
            wchar_t c = *(token.begin + i);
            if('\\' == c)
            {
                ++i;
                c = *(token.begin + i);
                switch(c)
                {
                case '"':
                case '/':
                case '\\':
                    decoded_str.push_back(c);
                    break;
                case 'b':
                    decoded_str.push_back('\b');
                    break;
                case 'f':
                    decoded_str.push_back('\f');
                    break;
                case 'n':
                    decoded_str.push_back('\n');
                    break;
                case 'r':
                    decoded_str.push_back('\r');
                    break;
                case 't':
                    decoded_str.push_back('\t');
                    break;
                case 'v':
                    decoded_str.push_back('\v');
                    break;

                case 'x':
                    decoded_str.push_back((HexDigitToInt(*(token.begin+i+1)) << 4) +
                        HexDigitToInt(*(token.begin+i+2)));
                    i += 2;
                    break;
                case 'u':
                    decoded_str.push_back((HexDigitToInt(*(token.begin+i+1)) << 12 ) +
                        (HexDigitToInt(*(token.begin+i+2)) << 8) +
                        (HexDigitToInt(*(token.begin+i+3)) << 4) +
                        HexDigitToInt(*(token.begin+i+4)));
                    i += 4;
                    break;

                default:
                    // ����ֻ����֤, ����ַ������Ϸ�, ˵��ParseStringToken����ȷ.
                    NOTREACHED();
                    return NULL;
                }
            }
            else
            {
                // ûת��.
                decoded_str.push_back(c);
            }
        }
        return Value::CreateStringValue(decoded_str);
    }

    JSONReader::Token JSONReader::ParseToken()
    {
        static const std::wstring kNullString(L"null");
        static const std::wstring kTrueString(L"true");
        static const std::wstring kFalseString(L"false");

        EatWhitespaceAndComments();

        Token token(Token::INVALID_TOKEN, 0, 0);
        switch(*json_pos_)
        {
        case '\0':
            token.type = Token::END_OF_INPUT;
            break;

        case 'n':
            if(NextStringMatch(kNullString))
            {
                token = Token(Token::NULL_TOKEN, json_pos_, 4);
            }
            break;

        case 't':
            if(NextStringMatch(kTrueString))
            {
                token = Token(Token::BOOL_TRUE, json_pos_, 4);
            }
            break;

        case 'f':
            if(NextStringMatch(kFalseString))
            {
                token = Token(Token::BOOL_FALSE, json_pos_, 5);
            }
            break;

        case '[':
            token = Token(Token::ARRAY_BEGIN, json_pos_, 1);
            break;

        case ']':
            token = Token(Token::ARRAY_END, json_pos_, 1);
            break;

        case ',':
            token = Token(Token::LIST_SEPARATOR, json_pos_, 1);
            break;

        case '{':
            token = Token(Token::OBJECT_BEGIN, json_pos_, 1);
            break;

        case '}':
            token = Token(Token::OBJECT_END, json_pos_, 1);
            break;

        case ':':
            token = Token(Token::OBJECT_PAIR_SEPARATOR, json_pos_, 1);
            break;

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        case '-':
            token = ParseNumberToken();
            break;

        case '"':
            token = ParseStringToken();
            break;
        }
        return token;
    }

    bool JSONReader::NextStringMatch(const std::wstring& str)
    {
        for(size_t i=0; i<str.length(); ++i)
        {
            if('\0' == *json_pos_)
            {
                return false;
            }
            if(*(json_pos_+i) != str[i])
            {
                return false;
            }
        }
        return true;
    }

    void JSONReader::EatWhitespaceAndComments()
    {
        while('\0' != *json_pos_)
        {
            switch(*json_pos_)
            {
            case ' ':
            case '\n':
            case '\r':
            case '\t':
                ++json_pos_;
                break;
            case '/':
                // TODO: RFC��û����, ���Կ���Ϊ���������.
                if(!EatComment())
                {
                    return;
                }
                break;
            default:
                // ���ǿհ�, �˳�.
                return;
            }
        }
    }

    bool JSONReader::EatComment()
    {
        if('/' != *json_pos_)
        {
            return false;
        }

        wchar_t next_char = *(json_pos_ + 1);
        if('/' == next_char)
        {
            // ��ע��, һֱ��ȡ��\n����\r.
            json_pos_ += 2;
            while('\0' != *json_pos_)
            {
                switch(*json_pos_)
                {
                case '\n':
                case '\r':
                    ++json_pos_;
                    return true;
                default:
                    ++json_pos_;
                }
            }
        }
        else if('*' == next_char)
        {
            // ��ע��, һֱ��ȡ��*/
            json_pos_ += 2;
            while('\0' != *json_pos_)
            {
                if('*'==*json_pos_ && '/'==*(json_pos_+1))
                {
                    json_pos_ += 2;
                    return true;
                }
                ++json_pos_;
            }
        }
        else
        {
            return false;
        }
        return true;
    }

    void JSONReader::SetErrorCode(JsonParseError error,
        const wchar_t* error_pos)
    {
        int line_number = 1;
        int column_number = 1;

        // ָ��������������к�.
        for(const wchar_t* pos=start_pos_; pos!=error_pos; ++pos)
        {
            if(*pos == '\0')
            {
                NOTREACHED();
                return;
            }

            if(*pos == '\n')
            {
                ++line_number;
                column_number = 1;
            }
            else
            {
                ++column_number;
            }
        }

        error_line_ = line_number;
        error_col_ = column_number;
        error_code_ = error;
    }

} //namespace base
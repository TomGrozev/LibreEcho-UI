#include "config_store.h"
#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int config_read(const char*p,char*b,size_t z){int fd;ssize_t n;if(!p||!b||z<2)return-1;fd=open(p,O_RDONLY);if(fd<0)return-1;n=read(fd,b,z-1);close(fd);if(n<0||(size_t)n>=z-1)return-1;b[n]=0;return(int)n;}
int config_write_atomic(const char*p,const char*b,size_t n){char tmp[512],bak[512];int fd;ssize_t w;if(!p||strlen(p)>450)return-1;snprintf(tmp,sizeof(tmp),"%s.tmp",p);snprintf(bak,sizeof(bak),"%s.bak",p);fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return-1;if(fchmod(fd,0600)){close(fd);unlink(tmp);return-1;}w=write(fd,b,n);if(w!=(ssize_t)n||fsync(fd)){close(fd);unlink(tmp);return-1;}if(close(fd)){unlink(tmp);return-1;}unlink(bak);if(chmod(p,0600)<0&&errno!=ENOENT){unlink(tmp);return-1;}if(link(p,bak)<0&&errno!=ENOENT){/* backup is best effort */}if(rename(tmp,p)){unlink(tmp);return-1;}chmod(p,0600);return 0;}
int config_copy_defaults(const char*from,const char*to){char b[16384];int n=config_read(from,b,sizeof(b));return n<0?-1:config_write_atomic(to,b,(size_t)n);}
/* Canonical 32-byte PSK encoding: 43 alphabet characters and exactly one '='.
 * The low two pad bits must be zero; accept empty only as unprovisioned. */
int config_esphome_key_valid(const char *key)
{
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;const char *last;
    if(!key)return 0;
    if(!key[0])return 1;
    if(strlen(key)!=44||key[43]!='=')return 0;
    for(i=0;i<43;i++)if(!strchr(alphabet,key[i]))return 0;
    last=strchr(alphabet,key[42]);return last&&((last-alphabet)&3)==0;
}
/* The same name contract as wake_model.h. config_store is linked by every
 * daemon, so the rule is restated here rather than pulling the model code into
 * each one. Stock "alexa_v0.1" and staged folder names both pass. */
static int wake_word_name_valid(const char *name)
{
    size_t i, length;
    if (!name) return 0;
    length = strlen(name);
    if (length == 0 || length >= 64 || name[0] == '.') return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') return 0;
    }
    return 1;
}

int config_esphome_read(const char *path, struct le_esphome_config *config)
{
    char data[16384];int n,field;
    if(!config)return -1;
    memset(config,0,sizeof(*config));snprintf(config->ha_protocol,sizeof(config->ha_protocol),"esphome");
    if(!path||!path[0])return 0;
    /* config_read may reject oversized input without an errno of its own.
     * Never mistake stale ENOENT for an absent key-bearing configuration. */
    errno=0;
    n=config_read(path,data,sizeof(data));
    if(n<0)return errno==ENOENT?0:-1;
    if(!json_valid_object(data,(size_t)n)||json_duplicate_key(data,(size_t)n,"esphome_noise_key"))return -1;
    field=json_get_string_top_level(data,"esphome_noise_key",config->esphome_noise_key,sizeof(config->esphome_noise_key));
    if(field<0||!config_esphome_key_valid(config->esphome_noise_key)){memset(config->esphome_noise_key,0,sizeof(config->esphome_noise_key));return -1;}
    if(json_duplicate_key(data,(size_t)n,"esphome_active_wake_word"))return -1;
    field=json_get_string_top_level(data,"esphome_active_wake_word",config->esphome_active_wake_word,sizeof(config->esphome_active_wake_word));
    if(field<0||(field>0&&config->esphome_active_wake_word[0]&&!wake_word_name_valid(config->esphome_active_wake_word)))return -1;
    config->esphome_active_wake_word_present=field>0;
    /* Owner option: invalid or absent means off, and is then dropped on the next save. */
    {int server=0;config->esphome_server_wake_present=json_get_bool(data,"esphome_server_wake",&server)>0;config->esphome_server_wake=config->esphome_server_wake_present&&server;}
    {int ble=0;config->esphome_bluetooth_proxy_present=json_get_bool(data,"esphome_bluetooth_proxy",&ble)>0;config->esphome_bluetooth_proxy=config->esphome_bluetooth_proxy_present&&ble;}
    return 0;
}
int config_set_top_level_string(const char *path, const char *field, const char *value)
{
    char old[16384], out[16384], needle[128], escaped[512];
    int n;
    char *start = NULL, *q, *end;
    size_t prefix;

    if (!path || !path[0] || !field || !field[0] || !value) return -1;
    n = config_read(path, old, sizeof(old));
    if (n < 0 || !json_valid_object(old, (size_t)n) ||
        json_duplicate_key(old, (size_t)n, field)) return -1;
    if (snprintf(needle, sizeof(needle), "\"%s\"", field) >= (int)sizeof(needle)) return -1;
    json_escape(escaped, sizeof(escaped), value);
    /* json_get_string_top_level identifies an unambiguous top-level string;
     * the same field is then replaced in place or appended before the brace. */
    {
        char test[512];
        int found = json_get_string_top_level(old, field, test, sizeof(test));
        if (found < 0) return -1;
        if (found == 1) {
            /* Re-locate the top-level occurrence by scanning with the same
               rules json_get_string_top_level used; the first quoted key at
               depth one whose text equals the field. */
            int d = 0;
            for (q = old; *q; q++) {
                char *key_end;
                size_t key_len;
                if (*q == '{' || *q == '[') { d++; continue; }
                if (*q == '}' || *q == ']') { d--; continue; }
                if (*q != '"') continue;
                key_end = q + 1;
                while (*key_end) {
                    if (*key_end == '\\') { if (!key_end[1]) return -1; key_end += 2; }
                    else if (*key_end == '"') break;
                    else key_end++;
                }
                if (!*key_end) return -1;
                key_len = (size_t)(key_end - q - 1);
                if (d == 1 && key_len == strlen(field) && !memcmp(q + 1, field, key_len)) {
                    char *colon = key_end + 1;
                    while (isspace((unsigned char)*colon)) colon++;
                    if (*colon != ':') return -1;
                    start = colon + 1;
                    while (isspace((unsigned char)*start)) start++;
                    if (*start != '"') return -1;
                    end = start + 1;
                    while (*end) {
                        if (*end == '\\') { if (!end[1]) return -1; end += 2; }
                        else if (*end == '"') { end++; break; }
                        else end++;
                    }
                    if (end[-1] != '"') return -1;
                    prefix = (size_t)(start - old);
                    {
                        int z = snprintf(out, sizeof(out), "%.*s\"%s\"%s",
                                         (int)prefix, old, escaped, end);
                        if (z < 0 || (size_t)z >= sizeof(out)) return -1;
                        return config_write_atomic(path, out, (size_t)z);
                    }
                }
                q = key_end;
            }
            return -1;
        }
    }
    end = old + n;
    while (end > old && isspace((unsigned char)end[-1])) end--;
    if (end == old || end[-1] != '}') return -1;
    prefix = (size_t)(end - old - 1);
    q = old + 1;
    while (isspace((unsigned char)*q)) q++;
    {
        int z = snprintf(out, sizeof(out), "%.*s%s\"%s\":\"%s\"}\n",
                         (int)prefix, old, *q == '}' ? "" : ",", field, escaped);
        if (z < 0 || (size_t)z >= sizeof(out)) return -1;
        return config_write_atomic(path, out, (size_t)z);
    }
}

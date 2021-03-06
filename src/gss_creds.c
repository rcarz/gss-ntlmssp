/*
   Copyright (C) 2013 Simo Sorce <simo@samba.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>

#include "gss_ntlmssp.h"

static int get_user_file_creds(struct gssntlm_name *name,
                               struct gssntlm_cred *cred)
{
    const char *envvar;
    char line[1024];
    char *dom, *usr, *pwd;
    char *p;
    bool found = false;
    FILE *f;
    int ret;

    /* use the same var used by Heimdal */
    envvar = getenv("NTLM_USER_FILE");
    if (envvar == NULL) return ENOENT;

    /* Use the same file format used by Heimdal in hope to achieve
     * some compatibility between implementations:
     * Each line is one entry like the following:
     * DOMAIN:USERNAME:PASSWORD */
    f = fopen(envvar, "r");
    if (!f) return errno;

    while(fgets(line, 1024, f)) {
        p = line;
        if (*p == '#') continue;
        dom = p;
        p = strchr(dom, ':');
        if (!p) continue;
        *p++ = '\0';
        usr = p;
        p = strchr(usr, ':');
        if (!p) continue;
        *p++ = '\0';
        pwd = p;
        strsep(&p, "\r\n");

        /* if no name is specified use the first found */
        if (name == NULL) {
            found = true;
            break;
        }

        if (name->data.user.domain) {
            if (!ntlm_casecmp(dom, name->data.user.domain)) continue;
        }
        if (name->data.user.name) {
            if (!ntlm_casecmp(usr, name->data.user.name)) continue;
        }
        /* all matched (NULLs in name are wildcards) */
        found = true;
        break;
    }

    fclose(f);

    if (!found) {
        return ENOENT;
    }

    cred->type = GSSNTLM_CRED_USER;
    cred->cred.user.user.type = GSSNTLM_NAME_USER;
    cred->cred.user.user.data.user.domain = strdup(dom);
    if (!cred->cred.user.user.data.user.domain) return ENOMEM;
    cred->cred.user.user.data.user.name = strdup(usr);
    if (!cred->cred.user.user.data.user.name) return ENOMEM;
    cred->cred.user.nt_hash.length = 16;

    ret = NTOWFv1(pwd, &cred->cred.user.nt_hash);
    if (ret) return ret;

    if (gssntlm_get_lm_compatibility_level() < 3) {
        cred->cred.user.lm_hash.length = 16;
        ret = LMOWFv1(pwd, &cred->cred.user.lm_hash);
        if (ret) return ret;
    }

    return 0;
}

static int get_server_creds(struct gssntlm_name *name,
                            struct gssntlm_cred *cred)
{
    gss_name_t gssname = NULL;
    gss_buffer_desc tmpbuf;
    uint32_t retmaj;
    uint32_t retmin;
    int ret;

    if (name == NULL) {
        tmpbuf.value = discard_const("");
        tmpbuf.length = 0;
        ret = 0;
        retmaj = gssntlm_import_name_by_mech(&retmin,
                                             &gssntlm_oid,
                                             &tmpbuf,
                                             GSS_C_NT_HOSTBASED_SERVICE,
                                             &gssname);
        if (retmaj) return retmin;

        name = (struct gssntlm_name *)gssname;
    }

    cred->type = GSSNTLM_CRED_SERVER;
    ret = gssntlm_copy_name(name, &cred->cred.server.name);
    gssntlm_int_release_name((struct gssntlm_name *)gssname);

    return ret;
}

static int hex_to_key(const char *hex, struct ntlm_key *key)
{
    const char *p;
    uint32_t i, j;
    uint8_t t;
    size_t len;

    len = strlen(hex);
    if (len != 32) return EINVAL;

    for (i = 0; i < 16; i++) {
        for (j = 0; j < 2; j++) {
            p = &hex[j + (i * 2)];
            if (*p >= '0' && *p <= '9') {
                t = (*p - '0');
            } else if (*p >= 'A' && *p <= 'F') {
                t = (*p - 'A' + 10);
            } else {
                return EINVAL;
            }
            if (j == 0) t = t << 4;
            key->data[i] = t;
        }
    }
    key->length = 16;
    return 0;
}

#define GENERIC_CS_PASSWORD "password"
/* To support in future, RC4 Key is NT hash */
#define KRB5_CS_CLI_KEYTAB_URN "client_keytab"
#define KRB5_CS_KEYTAB_URN "keytab"

static int get_creds_from_store(struct gssntlm_name *name,
                                struct gssntlm_cred *cred,
                                gss_const_key_value_set_t cred_store)
{
    uint32_t i;
    int ret;

    cred->type = GSSNTLM_CRED_NONE;

    if (name) {
        switch (name->type) {
        case GSSNTLM_NAME_NULL:
            cred->type = GSSNTLM_CRED_NONE;
            break;
        case GSSNTLM_NAME_ANON:
            cred->type = GSSNTLM_CRED_ANON;
            break;
        case GSSNTLM_NAME_USER:
            cred->type = GSSNTLM_CRED_USER;
            ret = gssntlm_copy_name(name, &cred->cred.user.user);
            break;
        case GSSNTLM_NAME_SERVER:
            cred->type = GSSNTLM_CRED_SERVER;
            ret = gssntlm_copy_name(name, &cred->cred.server.name);
            break;
        default:
            return EINVAL;
        }
    }

    /* so far only user options can be defined in the cred_store */
    if (cred->type != GSSNTLM_CRED_USER) return ENOENT;

    for (i = 0; i < cred_store->count; i++) {
        if (strcmp(cred_store->elements[i].key, GSS_NTLMSSP_CS_DOMAIN) == 0) {
            /* ignore duplicates */
            if (cred->cred.user.user.data.user.domain) continue;
            cred->cred.user.user.data.user.domain =
                                    strdup(cred_store->elements[i].value);
            if (!cred->cred.user.user.data.user.domain) return ENOMEM;
        }
        if (strcmp(cred_store->elements[i].key, GSS_NTLMSSP_CS_NTHASH) == 0) {
            /* ignore duplicates */
            if (cred->cred.user.nt_hash.length) continue;
            ret = hex_to_key(cred_store->elements[i].value,
                             &cred->cred.user.nt_hash);
            if (ret) return ret;
        }
        if ((strcmp(cred_store->elements[i].key, GSS_NTLMSSP_CS_PASSWORD) == 0) ||
            (strcmp(cred_store->elements[i].key, GENERIC_CS_PASSWORD) == 0)) {
            if (cred->cred.user.nt_hash.length) continue;
            cred->cred.user.nt_hash.length = 16;
            ret = NTOWFv1(cred_store->elements[i].value,
                          &cred->cred.user.nt_hash);

            if (gssntlm_get_lm_compatibility_level() < 3) {
                cred->cred.user.lm_hash.length = 16;
                ret = LMOWFv1(cred_store->elements[i].value,
                              &cred->cred.user.lm_hash);
                if (ret) return ret;
            }

            if (ret) return ret;
        }
    }

    /* TODO: should we call get_user_file_creds/get_server_creds if values are
     * not found ?
     */

    return 0;
}

static void gssntlm_copy_key(struct ntlm_key *dest, struct ntlm_key *src)
{
    memcpy(dest->data, src->data, src->length);
    dest->length = src->length;
}

int gssntlm_copy_creds(struct gssntlm_cred *in, struct gssntlm_cred *out)
{
    char *dom = NULL, *usr = NULL, *srv = NULL;
    int ret = 0;

    out->type = GSSNTLM_CRED_NONE;

    switch (in->type) {
    case GSSNTLM_CRED_NONE:
        break;
    case GSSNTLM_CRED_ANON:
        out->cred.anon.dummy = 1;
        break;
    case GSSNTLM_CRED_USER:
        ret = gssntlm_copy_name(&in->cred.user.user,
                                &out->cred.user.user);
        if (ret) goto done;
        gssntlm_copy_key(&out->cred.user.nt_hash,
                         &in->cred.user.nt_hash);
        gssntlm_copy_key(&out->cred.user.lm_hash,
                         &in->cred.user.lm_hash);
        break;
    case GSSNTLM_CRED_SERVER:
        ret = gssntlm_copy_name(&in->cred.server.name,
                                &out->cred.server.name);
        if (ret) goto done;
        break;
    case GSSNTLM_CRED_EXTERNAL:
        ret = gssntlm_copy_name(&in->cred.external.user,
                                &out->cred.external.user);
        if (ret) goto done;
        break;
    }
    out->type = in->type;

done:
    if (ret) {
        safefree(dom);
        safefree(usr);
        safefree(srv);
    }
    return ret;
}

void gssntlm_int_release_cred(struct gssntlm_cred *cred)
{
    if (!cred) return;

    switch (cred->type) {
    case GSSNTLM_CRED_NONE:
        break;
    case GSSNTLM_CRED_ANON:
        cred->cred.anon.dummy = 0;
        break;
    case GSSNTLM_CRED_USER:
        gssntlm_int_release_name(&cred->cred.user.user);
        safezero(cred->cred.user.nt_hash.data, 16);
        cred->cred.user.nt_hash.length = 0;
        safezero(cred->cred.user.lm_hash.data, 16);
        cred->cred.user.lm_hash.length = 0;
        break;
    case GSSNTLM_CRED_SERVER:
        gssntlm_int_release_name(&cred->cred.server.name);
        break;
    case GSSNTLM_CRED_EXTERNAL:
        gssntlm_int_release_name(&cred->cred.external.user);
        break;
    }
}

uint32_t gssntlm_acquire_cred_from(uint32_t *minor_status,
                                   gss_name_t desired_name,
                                   uint32_t time_req,
                                   gss_OID_set desired_mechs,
                                   gss_cred_usage_t cred_usage,
                                   gss_const_key_value_set_t cred_store,
                                   gss_cred_id_t *output_cred_handle,
                                   gss_OID_set *actual_mechs,
                                   uint32_t *time_rec)
{
    struct gssntlm_cred *cred;
    struct gssntlm_name *name;
    uint32_t retmaj;
    uint32_t retmin;

    name = (struct gssntlm_name *)desired_name;

    cred = calloc(1, sizeof(struct gssntlm_cred));
    if (!cred) {
        return GSSERRS(errno, GSS_S_FAILURE);
    }

    /* FIXME: should we split the cred union and allow GSS_C_BOTH ?
     * It may be possible to specify get server name from env and/or
     * user creds from cred store at the same time, etc .. */
    if (cred_usage == GSS_C_BOTH) {
        if (name == NULL) {
            cred_usage = GSS_C_ACCEPT;
        } else {
            switch (name->type) {
            case GSSNTLM_NAME_SERVER:
                cred_usage = GSS_C_ACCEPT;
                break;
            case GSSNTLM_NAME_USER:
            case GSSNTLM_NAME_ANON:
                cred_usage = GSS_C_INITIATE;
                break;
            default:
                set_GSSERRS(ERR_BADCRED, GSS_S_CRED_UNAVAIL);
                goto done;
            }
        }
    }

    if (cred_usage == GSS_C_INITIATE) {
        if (name != NULL && name->type != GSSNTLM_NAME_USER) {
            set_GSSERRS(ERR_NOUSRNAME, GSS_S_BAD_NAMETYPE);
            goto done;
        }

        if (cred_store != GSS_C_NO_CRED_STORE) {
            retmin = get_creds_from_store(name, cred, cred_store);
        } else {
            retmin = get_user_file_creds(name, cred);
            if (retmin) {
                retmin = external_get_creds(name, cred);
            }
        }
        if (retmin) {
            set_GSSERR(retmin);
            goto done;
        }
    } else if (cred_usage == GSS_C_ACCEPT) {
        if (name != NULL && name->type != GSSNTLM_NAME_SERVER) {
            set_GSSERRS(ERR_NOSRVNAME, GSS_S_BAD_NAMETYPE);
            goto done;
        }

        retmin = get_server_creds(name, cred);
        if (retmin) {
            set_GSSERR(retmin);
            goto done;
        }
    } else if (cred_usage == GSS_C_BOTH) {
        set_GSSERRS(ERR_NOTSUPPORTED, GSS_S_CRED_UNAVAIL);
        goto done;
    } else {
        set_GSSERRS(ERR_BADARG, GSS_S_CRED_UNAVAIL);
        goto done;
    }

    set_GSSERRS(0, GSS_S_COMPLETE);

done:
    if (retmaj) {
        uint32_t tmpmin;
        gssntlm_release_cred(&tmpmin, (gss_cred_id_t *)&cred);
    } else {
        *output_cred_handle = (gss_cred_id_t)cred;
        if (time_rec) *time_rec = GSS_C_INDEFINITE;
    }

    return GSSERR();
}

uint32_t gssntlm_acquire_cred(uint32_t *minor_status,
                              gss_name_t desired_name,
                              uint32_t time_req,
                              gss_OID_set desired_mechs,
                              gss_cred_usage_t cred_usage,
                              gss_cred_id_t *output_cred_handle,
                              gss_OID_set *actual_mechs,
                              uint32_t *time_rec)
{
    return gssntlm_acquire_cred_from(minor_status,
                                     desired_name,
                                     time_req,
                                     desired_mechs,
                                     cred_usage,
                                     GSS_C_NO_CRED_STORE,
                                     output_cred_handle,
                                     actual_mechs,
                                     time_rec);
}

uint32_t gssntlm_release_cred(uint32_t *minor_status,
                              gss_cred_id_t *cred_handle)
{
    *minor_status = 0;

    if (!cred_handle) return GSS_S_COMPLETE;

    gssntlm_int_release_cred((struct gssntlm_cred *)*cred_handle);
    safefree(*cred_handle);

    return GSS_S_COMPLETE;
}

uint32_t gssntlm_acquire_cred_with_password(uint32_t *minor_status,
                                            gss_name_t desired_name,
                                            gss_buffer_t password,
                                            uint32_t time_req,
                                            gss_OID_set desired_mechs,
                                            gss_cred_usage_t cred_usage,
                                            gss_cred_id_t *output_cred_handle,
                                            gss_OID_set *actual_mechs,
                                            uint32_t *time_rec)
{
    gss_key_value_element_desc element;
    gss_key_value_set_desc cred_store;

    element.key = GENERIC_CS_PASSWORD;
    element.value = (const char *)password->value;

    cred_store.count = 1;
    cred_store.elements = &element;

    return gssntlm_acquire_cred_from(minor_status,
                                     desired_name,
                                     time_req,
                                     desired_mechs,
                                     cred_usage,
                                     &cred_store,
                                     output_cred_handle,
                                     actual_mechs,
                                     time_rec);
}

uint32_t gssntlm_inquire_cred(uint32_t *minor_status,
                              gss_cred_id_t cred_handle,
                              gss_name_t *name,
                              uint32_t *lifetime,
                              gss_cred_usage_t *cred_usage,
                              gss_OID_set *mechanisms)
{
    struct gssntlm_cred *cred = (struct gssntlm_cred *)GSS_C_NO_CREDENTIAL;
    uint32_t retmin, retmaj;
    uint32_t maj, min;

    if (cred_handle == GSS_C_NO_CREDENTIAL) {
        maj = gssntlm_acquire_cred_from(&min,
                                        NULL, GSS_C_INDEFINITE,
                                        NULL, GSS_C_INITIATE,
                                        GSS_C_NO_CRED_STORE,
                                        (gss_cred_id_t *)&cred,
                                        NULL, NULL);
        if (maj) {
            set_GSSERRS(0, GSS_S_NO_CRED);
            goto done;
        }
    } else {
        cred = (struct gssntlm_cred *)cred_handle;
    }

    if (cred->type == GSSNTLM_CRED_NONE) {
        set_GSSERRS(ERR_BADARG, GSS_S_NO_CRED);
        goto done;
    }

    if (name) {
        switch (cred->type) {
        case GSSNTLM_CRED_NONE:
        case GSSNTLM_CRED_ANON:
            *name = GSS_C_NO_NAME;
            break;
        case GSSNTLM_CRED_USER:
            maj = gssntlm_duplicate_name(&min,
                                         (gss_name_t)&cred->cred.user.user,
                                         name);
            if (maj != GSS_S_COMPLETE) {
                set_GSSERRS(min, maj);
                goto done;
            }
            break;
        case GSSNTLM_CRED_SERVER:
            maj = gssntlm_duplicate_name(&min,
                                         (gss_name_t)&cred->cred.server.name,
                                         name);
            if (maj != GSS_S_COMPLETE) {
                set_GSSERRS(min, maj);
                goto done;
            }
            break;
        case GSSNTLM_CRED_EXTERNAL:
            maj = gssntlm_duplicate_name(&min,
                                        (gss_name_t)&cred->cred.external.user,
                                        name);
            if (maj != GSS_S_COMPLETE) {
                set_GSSERRS(min, maj);
                goto done;
            }
            break;
        }
    }

    if (lifetime) *lifetime = GSS_C_INDEFINITE;
    if (cred_usage) {
        if (cred->type == GSSNTLM_CRED_SERVER) {
            *cred_usage = GSS_C_ACCEPT;
        } else {
            *cred_usage = GSS_C_INITIATE;
        }
    }

    if (mechanisms) {
        maj = gss_create_empty_oid_set(&min, mechanisms);
        if (maj != GSS_S_COMPLETE) {
            set_GSSERRS(min, maj);
            gss_release_name(&min, name);
            goto done;
        }
        maj = gss_add_oid_set_member(&min,
                                     discard_const(&gssntlm_oid),
                                     mechanisms);
        if (maj != GSS_S_COMPLETE) {
            set_GSSERRS(min, maj);
            gss_release_oid_set(&min, mechanisms);
            gss_release_name(&min, name);
            goto done;
        }
    }

    set_GSSERRS(0, GSS_S_COMPLETE);

done:
    if (cred_handle == GSS_C_NO_CREDENTIAL) {
        gssntlm_release_cred(&min, (gss_cred_id_t *)&cred);
    }
    return GSSERR();
}

uint32_t gssntlm_inquire_cred_by_mech(uint32_t *minor_status,
                                      gss_cred_id_t cred_handle,
                                      gss_OID mech_type,
                                      gss_name_t *name,
                                      uint32_t *initiator_lifetime,
                                      uint32_t *acceptor_lifetime,
                                      gss_cred_usage_t *cred_usage)
{
    gss_cred_usage_t usage;
    uint32_t lifetime;
    uint32_t retmaj;
    uint32_t retmin;
    uint32_t maj, min;

    maj = gssntlm_inquire_cred(&min, cred_handle, name,
                               &lifetime, &usage, NULL);
    if (maj != GSS_S_COMPLETE) return GSSERRS(min, maj);

    switch (usage) {
    case GSS_C_INITIATE:
        if (initiator_lifetime) *initiator_lifetime = lifetime;
        if (acceptor_lifetime) *acceptor_lifetime = 0;
        break;
    case GSS_C_ACCEPT:
        if (initiator_lifetime) *initiator_lifetime = 0;
        if (acceptor_lifetime) *acceptor_lifetime = lifetime;
        break;
    case GSS_C_BOTH:
        if (initiator_lifetime) *initiator_lifetime = lifetime;
        if (acceptor_lifetime) *acceptor_lifetime = lifetime;
        break;
    default:
        return GSSERRS(ERR_BADARG, GSS_S_FAILURE);
    }

    if (cred_usage) *cred_usage = usage;
    return GSSERRS(0, GSS_S_COMPLETE);
}

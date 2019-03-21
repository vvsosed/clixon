/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * YANG module revision change management. 
 * See draft-wang-netmod-module-revision-management-01
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_err.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_log.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_xml_map.h"
#include "clixon_yang_module.h"
#include "clixon_yang_changelog.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"

#if 0
/*! Make a specific change
   <index>0001</index>
   <change-operation>create</change-operation>
   <data-definition>
      <target-node>
              /a:system/a:y;
    </target-node>
   </data-definition> 
*/
static int
upgrade_op(cxobj *x)
{
    int        retval = -1;

    xml_print(stderr, x);
    retval = 0;
    // done:
    return retval;
}

static int
upgrade_deleted(clicon_handle h,
		     char         *name,
		     cxobj        *xs)
{
    int        retval = -1;

    fprintf(stderr, "%s \"%s\" belongs to a removed module\n", __FUNCTION__, name);
    retval = 0;
    // done:
    return retval;
}

/*!
 * @param[in]  xs  Module state
 */
static int
upgrade_modified(clicon_handle h,
		 char         *name,
		 char         *namespace,
		 cxobj        *xs,
		 cxobj        *xch)
{
    int        retval = -1;
    char      *mname;
    yang_spec *yspec = NULL;
    yang_stmt *ymod;
    yang_stmt *yrev;
    char      *mrev;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;
    
    fprintf(stderr, "%s: \"%s\" belongs to an upgraded module\n", __FUNCTION__, name);
    yspec = clicon_dbspec_yang(h);

    /* We need module-name of XML since changelog uses that (change in changelog?)*/
    mname = xml_find_body(xs, "name");

    /* Look up system module (alt send it via argument) */
    if ((ymod = yang_find_module_by_name(yspec, mname)) == NULL)
	goto done;
    if ((yrev = yang_find((yang_node*)ymod, Y_REVISION, NULL)) == NULL)
	goto done;
    mrev = yrev->ys_argument;
    /* Look up in changelog */

    if (xpath_vec(xch, "module[name=\"%s\" and revision=\"%s\"]/revision-change-log",
		  &vec, &veclen, mname, mrev) < 0)
	goto done;
    /* Iterate through changelog */
    for (i=0; i<veclen; i++)
	if (upgrade_op(vec[i]) < 0)
	    goto done;

    retval = 0;
 done:
	if (vec)
	    free(vec);
    return retval;
}
#endif

/*! Automatic upgrade using changelog
 * @param[in]  h       Clicon handle 
 * @param[in]  xn      XML tree to be updated
 * @param[in]  modname Name of module
 * @param[in]  modns   Namespace of module (for info)
 * @param[in]  from    From revision on the form YYYYMMDD
 * @param[in]  to      To revision on the form YYYYMMDD (0 not in system)
 * @param[in]  arg     User argument given at rpc_callback_register() 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     1       OK
 * @retval     0       Invalid
 * @retval    -1       Error
 */
int
yang_changelog_upgrade(clicon_handle h,       
		       cxobj        *xn,      
		       char         *modname,
		       char         *modns,
		       uint32_t      from,
		       uint32_t      to,
		       void         *arg,     
		       cbuf         *cbret)
{
    //    cxobj     *xchlog; /* changelog */
    //    cxobj    **vec = NULL;
    //    size_t     veclen;

    if (!clicon_option_bool(h, "CLICON_YANG_CHANGELOG"))
	goto ok;
    /* Get changelog */
    //    xchlog = clicon_yang_changelog_get(h);
    /* Get changelog entries for module between from and to 
     * (if to=0 we may not know name, need to use namespace)
     */
#if 0
    if (xpath_vec(xchlog, "module[name=\"%s\" and revision=\"%s\"]/revision-change-log",
		  &vec, &veclen, modname, mrev) < 0)
	goto done;
#endif
 ok:
    return 1;
}

/*! Initialize module revision. read changelog, etc
 */
int
clixon_yang_changelog_init(clicon_handle h)
{
    int        retval = -1;
    char      *filename;
    int        fd = -1;
    cxobj     *xt = NULL;
    yang_spec *yspec;
    cbuf      *cbret = NULL;
    int        ret;

    yspec = clicon_dbspec_yang(h);
    if ((filename = clicon_option_str(h, "CLICON_YANG_CHANGELOG_FILE")) != NULL){
	if ((fd = open(filename, O_RDONLY)) < 0){
	    clicon_err(OE_UNIX, errno, "open(%s)", filename);
	    goto done;
	}    
	if (xml_parse_file(fd, NULL, yspec, &xt) < 0)
	    goto done;
	if (xml_rootchild(xt, 0, &xt) < 0)
	    goto done;
	if ((cbret = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	if ((ret = xml_yang_validate_all(xt, cbret)) < 0)
	    goto done;
	if (ret==1 && (ret = xml_yang_validate_add(xt, cbret)) < 0)
	    goto done;
	if (ret == 0){ /* validation failed */
	    clicon_err(OE_YANG, 0, "validation failed: %s", cbuf_get(cbret));
	    goto done;
	}   
	if (clicon_yang_changelog_set(h, xt) < 0)
	    goto done;
	xt = NULL;
    }
    retval = 0;
 done:
    if (fd != -1)
	close(fd);
    if (xt)
	xml_free(xt);
    if (cbret)
	cbuf_free(cbret);
    return retval;
}

/*----------------------------------------------------------------------------*/
#include "console/ConsoleMain.hh"
/*----------------------------------------------------------------------------*/

/* Owner Interface */
int 
com_chown (char* arg1) {
  XrdOucTokenizer subtokenizer(arg1);
  subtokenizer.GetLine();
  XrdOucString owner = subtokenizer.GetToken();
  XrdOucString option="";
  XrdOucString in = "mgm.cmd=chown";
  XrdOucString arg = "";

  if (owner.beginswith("-")) {
    option = owner;
    option.erase(0,1);
    owner = subtokenizer.GetToken();
    in += "&mgm.chown.option=";
    in += option;
  }

  XrdOucString path = subtokenizer.GetToken();

  if ( !path.length() || !owner.length() ) 
    goto com_chown_usage;

  path = abspath(path.c_str());

  in += "&mgm.path="; in += path;
  in += "&mgm.chown.owner="; in += owner;

  global_retc = output_result(client_admin_command(in));
  return (0);

 com_chown_usage:
  printf("Usage: chown [-r] <owner>[:<group>] <path>\n");
  printf("'[eos] chown ..' provides the change owner interface of EOS.\n");
  printf("<path> is the file/directory to modify, <owner> has to be a user id or user name. <group> is optional and has to be a group id or group name.\n");
  printf("Remark: EOS does access control on directory level - the '-r' option only applies to directories! It is not possible to set uid!=0 and gid=0!\n\n");
  printf("Options:\n");
  printf("                  -r : recursive\n");
  return (0);
}

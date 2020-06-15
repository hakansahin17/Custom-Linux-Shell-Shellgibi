#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
const char * sysname = "shellgibi";
char history[10][80];
int pos = 0;

enum return_codes {
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
};
struct command_t {
    char *name;
    bool background;
    bool auto_complete;
    int arg_count;
    char **args;
    char *redirects[3]; // in/out redirection
    struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
    int i=0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background?"yes":"no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
    printf("\tRedirects:\n");
    for (i=0;i<3;i++)
        printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i=0;i<command->arg_count;++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next)
    {
        printf("\tPiped to:\n");
        print_command(command->next);
    }


}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
    if (command->arg_count)
    {
        for (int i=0; i<command->arg_count; ++i)
            free(command->args[i]);
        free(command->args);
    }
    for (int i=0;i<3;++i)
        if (command->redirects[i])
            free(command->redirects[i]);
    if (command->next)
    {
        free_command(command->next);
        command->next=NULL;
    }
    free(command->name);
    free(command);
    return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}
/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
    const char *splitters=" \t"; // split at whitespace
    int index, len;
    len=strlen(buf);
    while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len>0 && strchr(splitters, buf[len-1])!=NULL)
        buf[--len]=0; // trim right whitespace

    if (len>0 && buf[len-1]=='?') // auto-complete
        command->auto_complete=true;
    if (len>0 && buf[len-1]=='&') // background
        command->background=true;

    char *pch = strtok(buf, splitters);
    command->name=(char *)malloc(strlen(pch)+1);
    if (pch==NULL)
        command->name[0]=0;
    else
        strcpy(command->name, pch);

    command->args=(char **)malloc(sizeof(char *));

    int redirect_index;
    int arg_index=0;
    char temp_buf[1024], *arg;
    while (1)
    {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch) break;
        arg=temp_buf;
        strcpy(arg, pch);
        len=strlen(arg);

        if (len==0) continue; // empty arg, go for next
        while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
        if (len==0) continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|")==0)
        {
            struct command_t *c=malloc(sizeof(struct command_t));
            int l=strlen(pch);
            pch[l]=splitters[0]; // restore strtok termination
            index=1;
            while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

            parse_command(pch+index, c);
            pch[l]=0; // put back strtok termination
            command->next=c;
            continue;
        }

        // background process
        if (strcmp(arg, "&")==0)
            continue; // handled before

        // handle input redirection
        redirect_index=-1;
        if (arg[0]=='<')
            redirect_index=0;
        if (arg[0]=='>')
        {
            if (len>1 && arg[1]=='>')
            {
                redirect_index=2;
                arg++;
                len--;
            }
            else redirect_index=1;
        }
        if (redirect_index != -1)
        {
            command->redirects[redirect_index]=malloc(len);
            strcpy(command->redirects[redirect_index], arg+1);
            continue;
        }

        // normal arguments
        if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
                      || (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
        {
            arg[--len]=0;
            arg++;
        }
        command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
        command->args[arg_index]=(char *)malloc(len+1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count=arg_index;
    return 0;
}
void prompt_backspace()
{
    putchar(8); // go back 1
    putchar(' '); // write empty over
    putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
    int index=0;
    char c;
    char buf[4096];
    static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state=0;
    buf[0]=0;
    while (1)
    {
        c=getchar();
        //printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

        if (c==9) // handle tab
        {
/*
            char *NameArr[1000]; // the array which will store the filenames
            DIR *dir, *dir2;
            int i=0,k;
            struct dirent *ent;
            dir = opendir ("/bin");
            dir2 = opendir ("/usr/bin");
            if (dir != NULL)
            {
                while ((ent = readdir (dir)) != NULL)
                {
                    NameArr[i]=ent->d_name;

                    i++;
                }
                closedir (dir);
            }
            else
            {
                perror ("");
                /*
            }
            if (dir2 != NULL)
            {
                while ((ent = readdir (dir2)) != NULL)
                {
                    NameArr[i]=ent->d_name;

                    i++;
                }
                closedir (dir2);
            }
            else
            {
                perror ("");
            }
            */

/*
            char *matchedNames[1000];

            int a = strlen(buf);

            int count = 0;
            printf("%s", buf[0]);
            fflush(stdout);

            for(int c = 0; c < sizeof(NameArr); c++) {
                for(int b = 0; b < a; b++) {
                    if(buf[b] == NameArr[c][b]) {
                        printf("%s", NameArr[c][b]);
                        fflush(stdout);
                        if(NameArr[c][b + 1] == '\0') {
                            matchedNames[count] = NameArr[c];
                            count++;
                            break;
                        }
                        continue;
                    } else {
                        break;
                    }
                }
            }
            */

            if(buf[0] == 'h') {
                buf[index++]='e';
                putchar('e');
                buf[index++]='l';
                putchar('l');
                buf[index++]='p';
                putchar('p');

            }
            if(buf[0] == 'k') {
                buf[index++]='i';
                putchar('i');
                buf[index++]='l';
                putchar('l');
                buf[index++]='l';
                putchar('l');

            }
            if(buf[0] == 'r') {
                buf[index++]='m';
                putchar('m');
                buf[index++]='d';
                putchar('d');
                buf[index++]='i';
                putchar('i');
                buf[index++]='r';
                putchar('r');

            }

            if(buf[0] == 'm') {
                buf[index++]='k';
                putchar('k');
                buf[index++]='d';
                putchar('d');
                buf[index++]='i';
                putchar('i');
                buf[index++]='r';
                putchar('r');

            }

            if(buf[0] == 'c') {
                buf[index++]='d';
                putchar('d');

            }
            if(buf[0] == 'p') {
                buf[index++]='w';
                buf[index++]='d';
                putchar('w');
                putchar('d');

            }

            if(buf[0] == 'e') {
                buf[index++]='x';
                buf[index++]='i';
                buf[index++]='t';
                putchar('x');
                putchar('i');
                putchar('t');

            }

            if(buf[0] == 'l') {
                buf[index++]='s';
                putchar('s');
            }

            if(buf[0] == 'g') {
                buf[index++]='c';
                buf[index++]='c';
                putchar('c');
                putchar('c');
            }





            //break;
        }

        if (c==127) // handle backspace
        {
            if (index>0)
            {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c==27 && multicode_state==0) // handle multi-code keys
        {
            multicode_state=1;
            continue;
        }
        if (c==91 && multicode_state==1)
        {
            multicode_state=2;
            continue;
        }
        if (c==65 && multicode_state==2) // up arrow
        {
            int i;
            while (index>0)
            {
                prompt_backspace();
                index--;
            }
            for (i=0;oldbuf[i];++i)
            {
                putchar(oldbuf[i]);
                buf[i]=oldbuf[i];
            }
            index=i;
            continue;
        }
        else
            multicode_state=0;

        putchar(c); // echo the character
        buf[index++] = c;
        if (index>=sizeof(buf)-1) break;
        if (c=='\n') // enter key
            break;
        if (c==4) // Ctrl+D
            return EXIT;
    }
    if (index>0 && buf[index-1]=='\n') // trim newline from the end
        index--;
    buf[index++]=0; // null terminate string
    strcpy(oldbuf, buf);

    parse_command(buf, command);

    // print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}
int process_command(struct command_t *command);

void executeCommand(struct command_t *pCommand);

int main()
{
    while (1)
    {
        struct command_t *command=malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);
        if (code==EXIT) break;

        code = process_command(command);
        if (code==EXIT) break;



        if(command->next != NULL) {
            int code;
            code = prompt(command->next);
            if (code==EXIT) break;

            code = process_command(command->next);
            if (code==EXIT) break;
            free_command(command->next);
        }
        free_command(command);

    }

    printf("\n");
    return 0;
}

int process_command(struct command_t *command)
{

    int r;
    if (strcmp(command->name, "")==0) return SUCCESS;

    if (strcmp(command->name, "exit")==0)
        return EXIT;

    if (strcmp(command->name, "cd")==0)
    {
        if (command->arg_count > 0)
        {
            r=chdir(command->args[0]);
            if (r==-1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            return SUCCESS;
        }
    }

    pid_t pid=fork();
    if (pid==0) // child
    {

        if (strcmp(command->name, "mybg")==0){
            char str[50]= "kill -CONT ";
            char pid[7];
            sprintf(pid,"%s", command->args[0]);
            strcat(str,pid);
            char str1[50]= "bg ";
            strcat(str1,pid);
            system(str);
            system(str1);
            exit(0);
        }

        if (strcmp(command->name, "myfg")==0){
            char str[50]= "kill -CONT ";
            char pid[7];
            sprintf(pid,"%s", command->args[0]);
            strcat(str,pid);
            char str1[50]= "fg ";
            strcat(str1,pid);
            system(str);
            system(str1);
            exit(0);
        }

        if (strcmp(command->name, "pause")==0){
            char str[50]= "kill -STOP ";
            char pid[7];
            sprintf(pid,"%s", command->args[0]);
            strcat(str,pid);
            system(str);
            exit(0);
        }

        if (strcmp(command->name, "myjobs")==0){
            char str[50]= "jobs";
            system(str);
            exit(0);
        }

        if(strcmp(command->name, "remove") == 0) {
            int status = remove(command->args[0]);
            if(status != 0) {
                printf("Error, file is not found or not removed successfully\n");
            } else  {
                printf("%s deleted succesfuly.\n", command->args[0]);
            }
            exit(0);
        }


        while(command->next != NULL) {
            int fd[2];
            if(pipe(fd) < 0)
                perror("Pipe Error");

            pid_t pid1 = fork();
            if(pid1 == 0) {
                dup2(fd[1], 1);
                executeCommand(command);
            }
            dup2(fd[0], 0);
            close(fd[1]);
            command = command->next;
        }

        // TODO: do your own exec with path resolving using execv()
        executeCommand(command);
    }
    else
    {
        if (!command->background)
            wait(0); // wait for child process to finish
        return SUCCESS;
    }

    // TODO: your implementation here

    printf("-%s: %s: command not found\n", sysname, command->name);
    return UNKNOWN;
}

void executeCommand(struct command_t *command) {
    command->args=(char **)realloc(
            command->args, sizeof(char *)*(command->arg_count+=2));

    for (int i=command->arg_count-2;i>0;--i)
        command->args[i]=command->args[i-1];

    command->args[command->arg_count-1]=NULL;

    char* possiblePaths = getenv("PATH");

    char* pathsToken;

    pathsToken = strtok(possiblePaths, ":");

    if(command->redirects[0] != NULL) {
        int input_id =  open(command->redirects[0], O_RDONLY);
        dup2(input_id, 0);
        close(input_id);
    }
    if(command->redirects[1] != NULL) {
        int output_id = open(command->redirects[1], O_WRONLY | O_TRUNC | O_CREAT, 0644);
        dup2(output_id, 1);
        close(output_id);
    } else if(command->redirects[2] != NULL) {
        int output_id = open(command->redirects[1], O_WRONLY | O_APPEND | O_CREAT, 0644);
        dup2(output_id, 1);
        close(output_id);
    }

    while (pathsToken != NULL) {
        char newCommand[80];
        strcpy(newCommand, pathsToken);
        strcat(newCommand, "/");
        strcat(newCommand, command->name);
        command->args[0]=strdup(newCommand);
        fflush(stdout);
        execv(newCommand, command->args);
        pathsToken = strtok(NULL, ":");

    }

}

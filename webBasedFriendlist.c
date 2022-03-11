/*
 * friendlist.c - [Starting code for] a web-based friend-graph manager.
 *
 * Based on:
 *  tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *      GET method to serve static and dynamic content.
 *   Tiny Web server
 *   Dave O'Hallaron
 *   Carnegie Mellon University
 *
 *   Additions made by Brighton Fox (u0981544)
 */
#include "csapp.h"
#include "dictionary.h"
#include "more_string.h"

static void *handleHTTP(void *fdp);
static dictionary_t *read_requesthdrs(rio_t *rp);
static void read_postquery(rio_t *rp, dictionary_t *headers, dictionary_t *d);
static void clienterror(int fd, char *cause, char *errnum,
                        char *shortmsg, char *longmsg);
static void print_stringdictionary(dictionary_t *d);

// Global tracker for all friendships
static dictionary_t *friendships;

// ^This^ will be our only shared resource between server-client threads (connections), so
// we just need one semaphore to keep it clean
static sem_t friendshipsSem;

// we need to know our port number to greatly simplify the introduce request
static int portNum;

// helper method to create the http ok message for returns
static void returnOK(size_t resLength, int fd);

// helper methods to handle the 4 input options for each client
static void friendsRequest(int fd, dictionary_t *query);
static void befriendRequest(int fd, dictionary_t *query);
static void unfriendRequest(int fd, dictionary_t *query);
static void introduceRequest(int fd, dictionary_t *query);

static void freeStringArray(char **strings);

// helper methods to edit the friendships in the backing dictionary
static void addFriends(char *user, char **newFriends);
static void removeFriends(char *user, char **oldFriends);

// helper method to get a string of all friends of the passed user
static char *getFriendsOf(char *user);

/**
 * @brief main method to initialize the backing dictionary and start the client-listening loop
 *
 * @param argc should be 2
 * @param argv [1] = port to open the server on
 * @return int
 */
int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // make the friends dictionary and its semaphore remember friends/users are
  // case-sensitive. Also note that free_dictionary is a custom deallocator for dictionarys
  friendships = make_dictionary(COMPARE_CASE_SENS, (void (*)(void *))free_dictionary);
  // initialize the semaphore with arg1=0 to signify it is only part of this program
  // and arg2=1 to start it as available
  Sem_init(&friendshipsSem, 0, 1);

  // open the specified port and save it for later reference
  listenfd = Open_listenfd(argv[1]);
  portNum = atoi(argv[1]);

  /* Don't kill the server if there's an error, because
     we want to survive errors due to a client. But we
     do want to report errors. */
  exit_on_error(0);

  /* Also, don't stop on broken connections: */
  Signal(SIGPIPE, SIG_IGN);

  pthread_t thread;
  int *connectionPointer;

  // infinite while loop that will listen for clients and assign them a thread when they connect
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
    if (connfd >= 0)
    {

      Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE,
                  port, MAXLINE, 0);
      // printf("Accepted connection from (%s, %s)\n", hostname, port);

      // allocate space for the connection file descriptor so the server can always reach it
      connectionPointer = malloc(sizeof(int));
      *connectionPointer = connfd; // "value at" connectionPointer is connfd (the connectionFileDescriptor)

      // give each connected client a thread to handle its requests
      Pthread_create(&thread, NULL, handleHTTP, connectionPointer);
      Pthread_detach(thread);
    }
  }
}

/**
 * @brief handles the requests from each client (on their own thread)
 *
 * @param fdp pointer to the file descriptor of the connection
 * @return void*
 */
void *handleHTTP(void *fdp)
{
  // to make the method work with threads, a pointer was needed,
  // it can be switched back to the object now
  int fd = *(int *)fdp;
  free(fdp);

  char buf[MAXLINE], *method, *uri, *version;
  rio_t rio;
  dictionary_t *headers, *query;

  /* Read request line and headers */
  Rio_readinitb(&rio, fd);
  if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
    return NULL;
  // printf("%s", buf);

  if (!parse_request_line(buf, &method, &uri, &version))
  {
    clienterror(fd, method, "400", "Bad Request",
                "Friendlist did not recognize the request");
  }
  else
  {
    if (strcasecmp(version, "HTTP/1.0") && strcasecmp(version, "HTTP/1.1"))
    {
      clienterror(fd, version, "501", "Not Implemented",
                  "Friendlist does not implement that version");
    }
    else if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
    {
      clienterror(fd, method, "501", "Not Implemented",
                  "Friendlist does not implement that method");
    }
    else
    {
      headers = read_requesthdrs(&rio);

      /* Parse all query arguments into a dictionary */
      query = make_dictionary(COMPARE_CASE_SENS, free);
      parse_uriquery(uri, query);
      if (!strcasecmp(method, "POST"))
        read_postquery(&rio, headers, query);

      // Check if any of the users/friends/servers had a special character in them and were classified
      // as a key in the query
      int i = 0;
      const char **queryKeys = dictionary_keys(query);
      while (queryKeys[i] != NULL)
      {
        if ((strcmp(queryKeys[i], "user") != 0) &&
            (strcmp(queryKeys[i], "friends") != 0) &&
            (strcmp(queryKeys[i], "host") != 0) &&
            (strcmp(queryKeys[i], "port") != 0) &&
            (strcmp(queryKeys[i], "friend") != 0))
        {
          if (strcmp(queryKeys[i - 1], "friends") != 0)
          {
            dictionary_set(query, queryKeys[i - 1], append_strings("&", queryKeys[i], NULL));
            dictionary_remove(query, queryKeys[i]);
          }
          else
          {
            dictionary_set(query, queryKeys[i - 1], append_strings("&", queryKeys[i], NULL));
            dictionary_remove(query, queryKeys[i]);
            // this should be different as it could have multiple paramters, but I couldn't figure out hoe to account for that
          }
        }
        i++;
      }

      free(queryKeys);

      // finally, determine the request type and call the appropriate helper method, all of the
      // methofds access the friendships dictionary, so it is best to lock it while any thread is in one
      // of the methods to avoid deadlock and data changes
      P(&friendshipsSem);

      if (starts_with("/friends", uri))
        friendsRequest(fd, query);
      else if (starts_with("/befriend", uri))
        befriendRequest(fd, query);
      else if (starts_with("/unfriend", uri))
        unfriendRequest(fd, query);
      else if (starts_with("/introduce", uri))
        introduceRequest(fd, query);

      V(&friendshipsSem);

      /* Clean up */
      free_dictionary(query);
      free_dictionary(headers);
    }

    /* Clean up status line */
    free(method);
    free(uri);
    free(version);
  }

  Close(fd);
  return NULL;
}

/*
 * read_requesthdrs - read HTTP request headers
 */
dictionary_t *read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];
  dictionary_t *d = make_dictionary(COMPARE_CASE_INSENS, free);

  Rio_readlineb(rp, buf, MAXLINE);
  // printf("%s", buf);
  while (strcmp(buf, "\r\n"))
  {
    Rio_readlineb(rp, buf, MAXLINE);
    // printf("%s", buf);
    parse_header_line(buf, d);
  }

  return d;
}

/*Read the POST header*/
void read_postquery(rio_t *rp, dictionary_t *headers, dictionary_t *dest)
{
  char *len_str, *type, *buffer;
  int len;

  len_str = dictionary_get(headers, "Content-Length");
  len = (len_str ? atoi(len_str) : 0);

  type = dictionary_get(headers, "Content-Type");

  buffer = malloc(len + 1);
  Rio_readnb(rp, buffer, len);
  buffer[len] = 0;

  if (!strcasecmp(type, "application/x-www-form-urlencoded"))
  {
    parse_query(buffer, dest);
  }

  free(buffer);
}

/*Make OK response header indicating a future write of len bytes*/
static char *ok_header(size_t len, const char *content_type)
{
  char *len_str, *header;

  header = append_strings("HTTP/1.0 200 OK\r\n",
                          "Server: Friendlist Web Server\r\n",
                          "Connection: close\r\n",
                          "Content-length: ", len_str = to_string(len), "\r\n",
                          "Content-type: ", content_type, "\r\n\r\n",
                          NULL);
  free(len_str);

  return header;
}

/**
 * @brief generates and sends the HTTP OK header
 *
 * @param resLength size of the response to follow the header
 * @param fd file descriptor containing the connection
 */
static void returnOK(size_t resLength, int fd)
{
  char *header;
  header = ok_header(resLength, "text/html; charset=utf-8");
  Rio_writen(fd, header, strlen(header));
  free(header);
}

/**
 * @brief generates and sends a list of all friends of a given user to the client
 *
 * @param fd file descriptor containing the connection
 * @param query pointer to the POST details
 */
static void friendsRequest(int fd, dictionary_t *query)
{
  // get user and their list of freinds
  char *user = dictionary_get(query, "user");
  char *resBody = getFriendsOf(user);

  // determine the size of the response message
  size_t resLength;
  if (resBody == NULL)
    resLength = 0;
  else
    resLength = strlen(resBody);

  // send HTTP OK adn the body containing the friends
  returnOK(resLength, fd);
  Rio_writen(fd, resBody, resLength);

  free(resBody);
}

/**
 * @brief adds the user and friends in the query as friends (symmetric) and returns a string of the
 *        users friends to the client
 *
 * @param fd file descriptor containing the connection
 * @param query pointer to the POST details
 */
static void befriendRequest(int fd, dictionary_t *query)
{
  // get user and the friends to add
  char *user = dictionary_get(query, "user");
  char **listOfFriends = split_string(dictionary_get(query, "friends"), '\n');

  // add the friends (symmetrically)
  addFriends(user, listOfFriends);

  // get size of response (friends of user)
  char *resBody = getFriendsOf(user);
  size_t resLength;
  if (resBody == NULL)
    resLength = 0;
  else
    resLength = strlen(resBody);

  // send HTTP OK adn the body containing the friends
  returnOK(resLength, fd);
  Rio_writen(fd, resBody, resLength);

  freeStringArray(listOfFriends);
  if (resBody)
    free(resBody);
}

/**
 * @brief removes the user and friends as friends (symettric) and return a string of the
 *        users friends to the client
 *
 * @param fd file descriptor containing the connection
 * @param query pointer to the POST details
 */
static void unfriendRequest(int fd, dictionary_t *query)
{
  // get user and the friends to add
  char *user = dictionary_get(query, "user");
  char **listOfFriends = split_string(dictionary_get(query, "friends"), '\n');

  // remove the firends (symmetric)
  removeFriends(user, listOfFriends);

  // get size of response (friends of user)
  char *resBody = getFriendsOf(user);
  size_t resLength;
  if (resBody == NULL)
    resLength = 0;
  else
    resLength = strlen(resBody);

  // send HTTP OK adn the body containing the friends
  returnOK(resLength, fd);
  Rio_writen(fd, resBody, resLength);

  freeStringArray(listOfFriends);
  if (resBody != NULL)
    free(resBody);
}

/**
 * @brief Add all friends of the POST friend to the user (symmetric). Friend may be on this server
 *        or another, but action here will result in the same
 *
 * @param fd file descriptor containing the connection
 * @param query pointer to the POST details
 */
static void introduceRequest(int fd, dictionary_t *query)
{
  // get all info from query to find friends to add to user
  char *user = dictionary_get(query, "user");
  char *host = dictionary_get(query, "host");
  char *port = dictionary_get(query, "port");
  char *friend = dictionary_get(query, "friend");
  char *friendAsList[2];
  friendAsList[0] = NULL;
  friendAsList[1] = NULL;

  // if the requested host is "localhost" and port matches this server, we can get the info here
  if ((strcmp(host, "localhost") == 0) && (portNum == atoi(port)))
  {
    // we can ignore addign users friends to themselves
    if (strcmp(user, friend) != 0)
    {
      // verify friend has any friends
      if (dictionary_get(friendships, friend))
      {
        // add friends of friends to user through direct access to the backing dicitonary
        addFriends(user, (char **)dictionary_keys(dictionary_get(friendships, friend)));
        friendAsList[0] = friend;
        addFriends(user, friendAsList);
      }
    }
  }
  // otherwise, create a HTTP request to the other server and get the collection of friends
  else
  {
    // create the HTTP request and open the connection to the server
    char *request = append_strings("GET /friends?user=", friend, " HTTP/1.1\r\n\r\n", NULL);
    size_t reqLength = strlen(request);
    int server_fd = Open_clientfd(host, port);

    // send our request to the server and read its response
    rio_t robustIO;
    char buffer[MAXLINE];
    Rio_writen(server_fd, request, reqLength);
    Rio_readinitb(&robustIO, server_fd);

    // the first line will be the status of the response
    Rio_readlineb(&robustIO, buffer, MAXLINE);
    char **html = split_string(buffer, ' ');

    // only proceed if the response is HTTP OK
    if (atoi(html[1]) == 200)
    {
      // step thorugh the reponse lines until you get to the names
      while (strcmp(buffer, "\r\n"))
        Rio_readlineb(&robustIO, buffer, MAXLINE);

      // add each friend in the response to user's friends after turning newline to null char
      while (Rio_readlineb(&robustIO, buffer, MAXLINE))
      {
        buffer[strlen(buffer) - 1] = '\0';
        friendAsList[0] = buffer;
        addFriends(user, friendAsList);
      }
    }

    freeStringArray(html);
    free(request);

    Close(server_fd);
  }

  // create and return HTTP repsonse
  char *resBody = append_strings(user, " introduced to friends of ", friend, NULL);
  size_t resLength = strlen(resBody);
  returnOK(resLength, fd);
  // printf("%s\n", body);
  Rio_writen(fd, resBody, resLength);

  free(resBody);
}

/*
 * clienterror - returns an error message to the client
 */
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
  size_t len;
  char *header, *body, *len_str;

  body = append_strings("<html><title>Friendlist Error</title>",
                        "<body bgcolor="
                        "ffffff"
                        ">\r\n",
                        errnum, " ", shortmsg,
                        "<p>", longmsg, ": ", cause,
                        "<hr><em>Friendlist Server</em>\r\n",
                        NULL);
  len = strlen(body);

  /* Print the HTTP response */
  header = append_strings("HTTP/1.0 ", errnum, " ", shortmsg, "\r\n",
                          "Content-type: text/html; charset=utf-8\r\n",
                          "Content-length: ", len_str = to_string(len), "\r\n\r\n",
                          NULL);
  free(len_str);

  Rio_writen(fd, header, strlen(header));
  Rio_writen(fd, body, len);

  free(header);
  free(body);
}

/**
 * @brief print a representation of the dictionary for debugging
 *
 */
static void print_stringdictionary(dictionary_t *d)
{
  int i, count;
  printf("Printing string dictionary\n");
  count = dictionary_count(d);
  for (i = 0; i < count; i++)
  {
    printf("%s=%s\n",
           dictionary_key(d, i),
           (const char *)dictionary_value(d, i));
  }
  printf("\n");
}

/**
 * @brief Get the Friends of the passed user
 *
 * @param user which users' friends to return
 * @return char* a string of friends seperated by newlines
 */
static char *getFriendsOf(char *user)
{
  // no need to handle users with no friends
  if (dictionary_get(friendships, user) == NULL)
    return NULL;

  // get array of friends from backing dictionary and combine into output
  const char **listOfFriends = dictionary_keys((dictionary_t *)dictionary_get(friendships, user));
  char *userFriends = join_strings((const char *const *)listOfFriends, '\n');

  free(listOfFriends);

  return userFriends;
}

/**
 * @brief add all friends to user (and vice-vera) in backing dicitonary
 *
 * @param user whom to connect the friends with
 * @param newFriends firends to connect to the user
 */
static void addFriends(char *user, char **newFriends)
{
  // if the user has no friends, instatiate them in the backing dictionary
  if (dictionary_get(friendships, user) == NULL)
    dictionary_set(friendships, user, make_dictionary(COMPARE_CASE_SENS, free));

  // step thorugh each friend and add them as a friend of the user asd well as
  // adding hte user as a firend of the friend
  int i = 0;
  char *newFriend;
  while (newFriends[i] != NULL)
  {
    newFriend = newFriends[i];
    // dont act user/friend as their own friend
    if (strcmp(newFriend, user) != 0)
    {
      // if the friend has no friends, instatiate them in the backing dictionary
      if (dictionary_get(friendships, newFriend) == NULL)
        dictionary_set(friendships, newFriend, make_dictionary(COMPARE_CASE_SENS, NULL));

      dictionary_set(dictionary_get(friendships, user), newFriend, NULL);
      dictionary_set(dictionary_get(friendships, newFriend), user, NULL);
    }
    i++;
  }
}

/**
 * @brief remove all friend connections between user and each friend in backing dictionary
 *
 * @param user whom to remove the friends from
 * @param oldFriends friends to remove from the user
 */
static void removeFriends(char *user, char **oldFriends)
{
  // if the user isn't in the dictionary, nothing to do
  if (dictionary_get(friendships, user) == NULL)
    return;

  // step thorugh each friend and remove them as a friend of the user as well as
  // removing the user as a friend of the friend
  int i = 0;
  char *oldFriend;
  while (oldFriends[i] != NULL)
  {
    oldFriend = oldFriends[i];
    // nothing to remove if the friend isnt in the dictioanry
    if (dictionary_get(friendships, oldFriend) != NULL)
    {
      dictionary_remove(dictionary_get(friendships, user), oldFriend);
      dictionary_remove(dictionary_get(friendships, oldFriend), user);
    }
    i++;
  }
}

/**
 * @brief small method to ensure string arrays are fully released in memory
 *
 * @param strings array of strings (char**) to release
 */
static void freeStringArray(char **strings)
{
  int i = 0;
  while (strings[i] != NULL)
    free(strings[i++]);

  free(strings);
}
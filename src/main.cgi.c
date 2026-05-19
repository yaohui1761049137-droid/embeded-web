/*
 * main.cgi - Main control page (requires valid session)
 */
#include "common.h"

int main() {
    char *token = get_cookie("boa_token");

    if (!token || !session_check(token)) {
        /* Not logged in, redirect to login */
        cgi_redirect("/index.html");
        return 0;
    }

    cgi_header("text/html; charset=utf-8");
    printf("<!DOCTYPE html>");
    printf("<html><head><meta charset='utf-8'>");
    printf("<title>Control Panel - LubanCat</title>");
    printf("<meta name='viewport' content='width=device-width, initial-scale=1'>");
    printf("<link rel='stylesheet' href='/style.css'>");
    printf("</head><body>");
    printf("<div class='container'>");
    printf("<h1>LubanCat Control Panel</h1>");
    printf("<p class='status'>System Ready</p>");
    printf("<hr>");
    printf("<button id='sendBtn' onclick='sendCommand()'>Send Data via Serial</button>");
    printf("<div id='result'></div>");
    printf("<hr>");
    printf("<a href='/cgi-bin/logout.cgi' class='logout-btn'>Logout</a>");
    printf("</div>");

    printf("<script>");
    printf("function sendCommand(){");
    printf("var btn=document.getElementById('sendBtn');");
    printf("var res=document.getElementById('result');");
    printf("btn.disabled=true;");
    printf("btn.textContent='Sending...';");
    printf("res.innerHTML='<p class=\"info\">Sending data via serial port...</p>';");
    printf("var xhr=new XMLHttpRequest();");
    printf("xhr.open('POST','/cgi-bin/action.cgi',true);");
    printf("xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');");
    printf("xhr.onload=function(){");
    printf("if(xhr.status==200){");
    printf("try{var r=JSON.parse(xhr.responseText);");
    printf("if(r.status=='ok')res.innerHTML='<p class=\"success\">Data sent: '+r.message+'</p>';");
    printf("else res.innerHTML='<p class=\"error\">Error: '+r.message+'</p>';");
    printf("}catch(e){res.innerHTML='<p class=\"error\">Parse error</p>';}");
    printf("}else{res.innerHTML='<p class=\"error\">HTTP '+xhr.status+'</p>';}");
    printf("btn.disabled=false;");
    printf("btn.textContent='Send Data via Serial';");
    printf("};");
    printf("xhr.onerror=function(){");
    printf("res.innerHTML='<p class=\"error\">Network error</p>';");
    printf("btn.disabled=false;");
    printf("btn.textContent='Send Data via Serial';");
    printf("};");
    printf("xhr.send('action=send');");
    printf("}");
    printf("</script>");

    printf("</body></html>");
    return 0;
}

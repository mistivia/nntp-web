# nntplib in deprecated in Python 3.13
# Use https://pypi.org/project/standard-nntplib/ instead
import nntplib
import email
from email import policy
from email.parser import BytesParser
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver
import socket
import base64
import re
import urllib.parse
import sys
from collections import deque

NNTP_SERVER = 'nntp.example.org'
NNTP_PORT = 119
GROUP_NAME = 'sharknews'
PAGE_SIZE = 25
LISTEN_PORT = 8000

class NNTPClient:
    def __init__(self):
        self.conn = None
        
    def connect(self):
        self.conn = nntplib.NNTP(NNTP_SERVER, NNTP_PORT)
        
    def get_group_overview(self, group_name, start, end):
        if not self.conn:
            self.connect()
            self.conn.group(GROUP_NAME)
        resp, count, first, last, name = self.conn.group(group_name)
        if start < first:
            start = first
        if end > last:
            end = last
            
        resp, overviews = self.conn.over((start, end))
        return overviews
    
    def get_article(self, article_id):
        if not self.conn:
            self.connect()
            self.conn.group(GROUP_NAME)
        resp, info = self.conn.article(article_id)
        return b'\n'.join(info.lines)
    
    def quit(self):
        if self.conn:
            self.conn.quit()
            self.conn = None

from email.header import decode_header

def decode_email_subject(subject):
    decoded_parts = decode_header(subject)
    
    decoded_strings = []
    for part, encoding in decoded_parts:
        if isinstance(part, bytes):
            try:
                charset = encoding or 'utf-8'
                decoded_strings.append(part.decode(charset, errors='replace'))
            except (LookupError, UnicodeDecodeError):
                decoded_strings.append(part.decode('latin-1', errors='replace'))
        else:
            decoded_strings.append(part)
    return ''.join(decoded_strings)


class NNTPRequestHandler(BaseHTTPRequestHandler):
    nntp_client = NNTPClient()
    
    def do_GET(self):
        try:
            if urllib.parse.urlparse(self.path).path == '/':
                self.show_index()
            elif self.path.startswith('/article/'):
                article_id = self.path.split('/')[-1]
                self.show_article(article_id)
            elif self.path.startswith('/attachment/'):
                parts = self.path.split('/')
                article_id = parts[2]
                part_id = int(parts[3])
                self.download_attachment(article_id, part_id)
            else:
                self.send_error(404)
        except nntplib.NNTPTemporaryError as e:
            self.send_error(502, f"NNTP Error: {str(e)}")
        except Exception as e:
            self.send_error(500, f"Server Error: {str(e)}")
    
    def show_index(self):
        page = 1
        query = urllib.parse.urlparse(self.path).query
        if query:
            params = urllib.parse.parse_qs(query)
            page = int(params.get('page', [1])[0])
        
        if not self.nntp_client.conn:
            self.nntp_client.connect()
        resp, count, first, last, name = self.nntp_client.conn.group(GROUP_NAME)
        self.nntp_client.quit()
        
        total_pages = (last - first) // PAGE_SIZE + 1
        start = last - (page - 1) * PAGE_SIZE
        end = start - PAGE_SIZE + 1
        
        if end < first:
            end = first
        
        overviews = self.nntp_client.get_group_overview(GROUP_NAME, end, start)
        
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()
        
        pagination = f'<div class="pagination">'
        if page > 1:
            pagination += f'<a href="/?page={page-1}">Prev</a> '
        pagination += f'<span>Page {page} / {total_pages}</span>'
        if page < total_pages:
            pagination += f' <a href="/?page={page+1}">Next</a>'
        pagination += '</div>'
        
        articles_html = '<ul class="article-list">'
        for art_id, overview in reversed(overviews):
            subject = overview.get('subject', 'Not Subject')
            subject = decode_email_subject(subject)
            author = overview.get('from', 'Anonymous')
            author = decode_email_subject(author)
            date = overview.get('date', 'Unknown Date')
            articles_html += f'''
                <li>
                    <a href="article/{art_id}">{subject}</a>
                    <div class="meta">Author: {author} | Date: {date}</div>
                </li>
            '''
        articles_html += '</ul>'
        
        html = f'''
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="utf-8">
            <title>{GROUP_NAME} - NNTP Reader</title>
            <style>
                body {{ font-family: sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }}
                h1 {{ color: #333; }}
                a {{ text-decoration: none; color: #0066cc; }}
                .article-list {{ list-style: none; padding: 0; }}
                .article-list li {{ border-bottom: 1px solid #eee; padding: 10px 0; }}
                .article-list a {{ text-decoration: none; color: #0066cc; font-size: 1.1em; }}
                .article-list a:hover {{ text-decoration: underline; }}
                .meta {{ color: #666; font-size: 0.9em; }}
                .pagination {{ margin: 20px 0; text-align: center; }}
                .pagination a, .pagination span {{ margin: 0 5px; }}
                .article-header {{ background-color: #f5f5f5; padding: 15px; border-radius: 5px; }}
                .attachments {{ margin-top: 20px; border-top: 1px dashed #ccc; padding-top: 10px; }}
                .attachment {{ margin-bottom: 10px; }}
            </style>
        </head>
        <body>
            <h1>{GROUP_NAME} Articles</h1>
            {pagination}
            {articles_html}
            {pagination}
        </body>
        </html>
        '''
        self.wfile.write(html.encode('utf-8'))
    
    def show_article(self, article_id):
        raw_article = self.nntp_client.get_article(article_id)
        msg = BytesParser(policy=policy.default).parsebytes(raw_article)
        
        subject = msg.get('subject', 'No Subject')
        author = msg.get('from', 'Anonymous')
        date = msg.get('date', 'Unknown Date')
        
        body_html = ''
        attachments = []
        part_id = 0
        
        if msg.is_multipart():
            for part in msg.iter_parts():
                content_type = part.get_content_type()
                if content_type.startswith('text/'):
                    charset = part.get_content_charset('utf-8')
                    body_html += part.get_payload(decode=True).decode(charset, errors='replace')
                else:
                    attachments.append((part_id, part))
                    part_id += 1
        else:
            charset = msg.get_content_charset('utf-8')
            body_html = msg.get_payload(decode=True).decode(charset, errors='replace')
        
        body_html = '<pre>' + body_html.replace('\n', '<br>') + '</pre>'
        
        attachments_html = ''
        if attachments:
            attachments_html = '<div class="attachments"><h3>Attachments</h3>'
            for pid, part in attachments:
                filename = part.get_filename('Unnamed')
                content_type = part.get_content_type()
                
                if content_type.startswith('image/'):
                    # Embedded Image
                    img_data = base64.b64encode(part.get_payload(decode=True)).decode('ascii')
                    attachments_html += f'''
                    <div class="attachment">
                        <strong>{filename}</strong> (Image)<br>
                        <img src="data:{content_type};base64,{img_data}" style="max-width: 100%;">
                    </div>
                    '''
                else:
                    attachments_html += f'''
                    <div class="attachment">
                        <a href="../attachment/{article_id}/{pid}">{filename}</a>
                        ({content_type}, {len(part.get_payload(decode=True))} bytes)
                    </div>
                    '''
            attachments_html += '</div>'
        
        html = f'''
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="utf-8">
            <title>{subject}</title>
            <style>
                body {{ font-family: sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }}
                .article-header {{ background-color: #f5f5f5; padding: 15px; border-radius: 5px; margin-bottom: 20px; }}
                .back-link {{ margin-bottom: 20px; display: block; }}
                pre {{ font-size: 12px; white-space: pre-wrap; background-color: #f8f8f8; padding: 10px; border-radius: 3px; }}
            </style>
        </head>
        <body>
            <a class="back-link" href="../">&larr; Back to list</a>
            <div class="article-header">
                <h1>{subject}</h1>
                <div><strong>Author:</strong> {author}</div>
                <div><strong>Date:</strong> {date}</div>
            </div>
            <div class="article-body">
                {body_html}
            </div>
            {attachments_html}
        </body>
        </html>
        '''
        
        self.send_response(200)
        self.send_header('Content-type', 'text/html')
        self.end_headers()
        self.wfile.write(html.encode('utf-8'))
    
    def download_attachment(self, article_id, part_id):
        raw_article = self.nntp_client.get_article(article_id)
        msg = BytesParser(policy=policy.default).parsebytes(raw_article)
        
        attachment = None
        part_index = 0
        
        if msg.is_multipart():
            for part in msg.iter_parts():
                if not part.is_multipart() and not part.get_content_type().startswith('text/'):
                    if part_index == part_id:
                        attachment = part
                        break
                    part_index += 1
        
        if not attachment:
            self.send_error(404, "Attachment not found")
            return
        
        filename = attachment.get_filename('attachment.bin')
        content = attachment.get_payload(decode=True)
        
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Disposition', f'attachment; filename="{filename}"')
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        self.wfile.write(content)

class ReusableAddressHTTPServer(HTTPServer):
    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        super().server_bind()

def run_server(port=8000):
    server_address = ('', port)
    httpd = ReusableAddressHTTPServer(server_address, NNTPRequestHandler)
    print(f"Start NNTP reader at http://localhost:{port}")
    print("Press Ctrl+C to stop")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nServer is shutting down...")
        httpd.server_close()
        sys.exit(0)

if __name__ == '__main__':
    run_server(port=LISTEN_PORT)


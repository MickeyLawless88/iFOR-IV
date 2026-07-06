PROGRAM BALL
        DIMENSION X(100), Y(100), Z(100),
     >            TIME(100), SYMBOL(121)
        INTEGER DOT, BLANK, STAR
        DATA DOT,BLANK,STAR/'.',' ','*'/
100     FORMAT(4F8.3,I3)        
200     FORMAT(' COMPUTER SIMULATION OF BOUNCING BALL PROBLEM'//,
     >         'HEIGHT =',F6.2,' FT',7X,'HORIZONTAL VELOCITY =',
     >          F6.2,' FT/SEC',7X,'TIME INCREMENT =',F6.3,' SEC'//
     >          ,'   BOUNCE COEFFICIENT=',F5.3,7X,
     >          'NUMBER OF BOUNCES =',I3//)
210     FORMAT('   HORIZONTAL DISTANCE TRAVELLED=',F7.3,' FT',
     >          10X,'TIME REQUIRED =',F7.3,' SECS'//)
220     FORMAT('    T=',F6.3,5X,'X=',F7.3,5X,'Y=',F6.3,5X,'Z=',F8.3)
230     FORMAT('  GRAPHICAL SOLUTION TO THE BOUNCING BALL PROBLEM'//)
240     FORMAT(5X,121A1)
250     FORMAT(5X,A1)
1       READ(*,100) HEIGHT, V, DT, COEFF, N
        WRITE(*,200) HEIGHT, V, DT, COEFF, N
C * INITIALIZE PARAMETERS *
        I = 1
        IB = 0
        TIME( I ) = 0.
        X( I ) = 0.
        Y( I ) = HEIGHT
        Z( I ) = 0.
        G = 32.2
C * COMPUTE H.DISPLACEMENT, V. VELOCITY *
        DO 2 I = 1, 99
           TIME( I + 1 ) = TIME( I ) + DT
           X( I + 1 ) = X( I ) + V * DT
           Z( I + 1 ) = Z( I ) - G * DT
           Y( I + 1 ) = Y( I ) + .5 * 
     >                ( Z( I ) + Z( I + 1 ) ) * DT
C * TEST TO SEE IF BALL HITS GROUND IN THIS *
C *             TIME INCREMENT              *
           IF( Y( I + 1 ) .GT. 0 ) GOTO 2
C * BALL HITS GROUND, TEST FOR REQUIRED #  *
C *             OF BOUNCES                 *
           IF( IB .EQ. N ) GOTO 3
C * LET IT BOUNCE *
           DTSTAR = DT * Y( I ) / ( Y( I ) - Y( I + 1 ) )
           ZSTAR = -COEFF * ( Z( I ) - G * DTSTAR )
           Y( I + 1 ) = .5 * ( ZSTAR + Z( I + 1 ) )
     >                                 *( DT - DTSTAR )
           IB = IB + 1
2       CONTINUE
3       DTSTAR = DT * Y( I ) / ( Y( I ) - Y( I + 1 ) )
        TIME( I + 1 ) = TIME( I ) + DTSTAR
        TSTAR = TIME( I + 1 )
        X( I + 1 ) = X( I ) + DTSTAR
        Y( I + 1 ) = 0.
        Z( I + 1 ) = Z( I ) - G * DTSTAR
        DIST = X( I + 1 )
        I1 = I + 1
C * PRINT NUMERICAL OUTPUT *
4       WRITE(*,210) DIST, TSTAR
        WRITE(*,220) (TIME(I),X(I),Y(I),Z(I),I=1,I1)
C * PRINT GRAPHICAL OUTPUT *
        WRITE(*,230)
        DO 5 J = 1, 120
5       SYMBOL( J ) = DOT
        SYMBOL( 121 ) = STAR
        WRITE(*,240 ) SYMBOL
        DO 7 I = 2, I1
           DO 6 J = 2, 121
6          SYMBOL( J ) = BLANK
           SYMBOL( 1 ) = DOT
           ISTAR = IFIX( 120. * Y( I ) / Y( 1 ) ) + 1
           SYMBOL( ISTAR ) = STAR
           WRITE(*,250) DOT, DOT
7       WRITE(*,240) SYMBOL
        STOP
        END